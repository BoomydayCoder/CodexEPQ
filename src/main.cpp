#include "ast.hpp"
#include "build_id.hpp"
#include "emit.hpp"
#include "quick.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

extern char** environ;
namespace fs = std::filesystem;

namespace {

constexpr char usage[] =
    "Usage: epq SOURCE                 run a source file\n"
    "       epq jit SOURCE             compile and execute in memory\n"
    "       epq native SOURCE          build, cache and run an optimized executable\n"
    "       epq build SOURCE -o FILE   build a standalone executable\n"
    "       epq emit SOURCE            write generated C++ to stdout\n";

[[noreturn]] void system_error(const std::string& action, int code = errno) {
    throw std::system_error(code, std::generic_category(), action);
}

std::string read_source(const fs::path& path) {
    if (path == "-")
        return {std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    std::ifstream file(path, std::ios::binary);
    if (!file) system_error("open " + path.string());
    if (!fs::is_regular_file(path))
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    file.seekg(0, std::ios::end);
    auto length = file.tellg();
    if (length < 0) throw std::runtime_error("unable to measure " + path.string());
    std::string source(static_cast<std::size_t>(length), '\0');
    file.seekg(0);
    if (!file.read(source.data(), length))
        throw std::runtime_error("unable to read " + path.string());
    return source;
}

fs::path driver_path() {
    std::vector<char> name(1024);
    uint32_t size = static_cast<uint32_t>(name.size());
    if (_NSGetExecutablePath(name.data(), &size) != 0) {
        name.resize(size);
        if (_NSGetExecutablePath(name.data(), &size) != 0)
            throw std::runtime_error("unable to locate the epq executable");
    }
    return fs::canonical(name.data());
}

class BuildDirectory {
public:
    explicit BuildDirectory(const fs::path& parent) {
        auto pattern = (parent / ".epq-build-XXXXXX").string();
        if (!mkdtemp(pattern.data())) system_error("create build directory in " + parent.string());
        path = std::move(pattern);
    }
    ~BuildDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

void invoke_compiler(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t child;
    int error = posix_spawn(&child, argv.front(), nullptr, nullptr, argv.data(), environ);
    if (error) system_error("start Apple Clang", error);
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) system_error("wait for Apple Clang");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("native compilation failed");
}

void build(const epq::Node& program, const fs::path& output) {
    const auto cpp = epq::emit(program);
    const auto root = driver_path().parent_path();
    BuildDirectory temp(output.parent_path());
    const auto source = temp.path / "program.cpp";
    {
        std::ofstream file(source, std::ios::binary);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << cpp;
        file.close();
    }
    const auto binary = temp.path / "program";
    invoke_compiler({"/usr/bin/clang++", "-std=c++17", "-O3", "-DNDEBUG", "-fwrapv",
                     "-mcpu=native", "-I", (root / "build/runtime").string(), "-include-pch",
                     (root / "build/runtime.pch").string(), source.string(),
                     (root / "build/epq_runtime.o").string(), "-Wl,-dead_strip,-stack_size,0x20000000",
                     "-o", binary.string()});
    fs::rename(binary, output);
}

fs::path cache_dir() {
    if (const char* override = std::getenv("EPQ_CACHE_DIR"); override && *override)
        return fs::absolute(override);
    // macOS gives each user a private directory that can be purged between logins.
    size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
    if (!size) throw std::runtime_error("unable to locate the user temporary directory");
    std::vector<char> path(size);
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, path.data(), size) != size)
        throw std::runtime_error("unable to locate the user temporary directory");
    return fs::path(path.data()) / "epq-native";
}

bool cache_ready(const fs::path& dir, bool create) {
    if (create) {
        fs::create_directories(dir.parent_path());
        if (mkdir(dir.c_str(), 0700) < 0 && errno != EEXIST)
            system_error("create cache " + dir.string());
    }
    struct stat info{};
    if (lstat(dir.c_str(), &info) < 0) {
        if (!create && errno == ENOENT) return false;
        system_error("inspect cache " + dir.string());
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0022))
        throw std::runtime_error("EPQ cache must be a directory owned by you without group or public write access");
    return true;
}

std::string cache_key(std::string_view source) {
    CC_SHA256_CTX state;
    CC_SHA256_Init(&state);
    CC_SHA256_Update(&state, EPQ_BUILD_ID, sizeof(EPQ_BUILD_ID));
    while (!source.empty()) {
        auto count = std::min<std::size_t>(source.size(), std::numeric_limits<CC_LONG>::max());
        CC_SHA256_Update(&state, source.data(), static_cast<CC_LONG>(count));
        source.remove_prefix(count);
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &state);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(2 * sizeof(digest));
    for (auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

class CacheLock {
public:
    explicit CacheLock(const fs::path& path) {
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) system_error("open cache lock");
        while (flock(fd, LOCK_EX) < 0) {
            if (errno != EINTR) {
                int error = errno;
                close(fd);
                system_error("lock cache", error);
            }
        }
    }
    ~CacheLock() { close(fd); }
private:
    int fd;
};

[[noreturn]] void execute(const fs::path& path) {
    execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
    system_error("execute " + path.string());
}

int run_source(const std::string& source, bool native) {
    const auto dir = cache_dir();
    if (cache_ready(dir, native)) {
        const auto binary = dir / cache_key(source);
        if (access(binary.c_str(), X_OK) == 0) execute(binary);
        if (native) {
            CacheLock lock(binary.string() + ".lock");
            if (access(binary.c_str(), X_OK) != 0) build(epq::parse(source), binary);
            execute(binary); // The cache lock closes on exec.
        }
    }
    return epq::quick_run(epq::parse(source));
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view command = argc > 1 ? argv[1] : "";
        if (argc == 2 && (command == "-h" || command == "--help")) {
            std::cout << usage;
            return 0;
        }
        if (argc == 2) return run_source(read_source(argv[1]), false);
        if (argc == 3 && command == "run") return run_source(read_source(argv[2]), false);
        if (argc == 3 && command == "native") return run_source(read_source(argv[2]), true);
        if (argc == 3 && (command == "jit" || command == "quick"))
            return epq::quick_run(epq::parse(read_source(argv[2])));
        if (argc == 3 && command == "emit") {
            std::cout << epq::emit(epq::parse(read_source(argv[2])));
            return 0;
        }
        if (argc == 5 && command == "build" && std::string_view(argv[3]) == "-o") {
            const fs::path output = fs::absolute(argv[4]);
            if (fs::exists(output) && ((fs::path(argv[2]) != "-" && fs::equivalent(output, argv[2])) ||
                                       fs::equivalent(output, driver_path())))
                throw std::runtime_error("output would overwrite the input or the compiler");
            build(epq::parse(read_source(argv[2])), output);
            return 0;
        }
        std::cerr << usage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "epq: " << error.what() << '\n';
        return 1;
    }
}
