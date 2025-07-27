#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

const std::string BASE_DIR = "/bit";
std::string root = "";
namespace fs = std::filesystem;
fs::path base = BASE_DIR;
using json = nlohmann::json;

struct Package {
    std::string name;
    std::vector<std::string> dependencies;
    std::string version;
    std::string commands;
    std::string url;
    std::string root;
};

void prompt_help() {
    std::cout << "\n\u2753 Help:\n\n\U0001F4E6 Packages:\n"
              << "- install <package> - Install a package.\n"
              << "- remove <package> - Remove a package.\n"
              << "- update - Update all packages.\n\n"
              << "\U0001F310 Remotes:\n"
              << "- remote-add <url> - Add a remote from URL.\n"
              << "- remote-add ppa:<profile>/<ppa> - Add a PPA.\n\n"
              << "\U0001F512 Locking:\n"
              << "- lock - Lock BitPuppy (block usage)\n"
              << "- unlock - Unlock BitPuppy\n\n";
}

void add_remote(const std::string& urlArg, const std::string& name, const std::vector<std::string>& channels) {
    fs::path dir = "/bit/Chocobitpup/remotes/" + name;
    fs::create_directories(dir);

    std::ofstream list(dir / "remote.choco.list", std::ios::app);
    list << "choco " << urlArg << " " << name;
    for (const auto& c : channels) list << " " << c;
    list << "\n";

    std::cout << "\u2705 Remote added to " << dir << "\n";
}

std::string detect_arch() {
    std::string result;
    FILE* pipe = popen("uname -m", "r");
    if (!pipe) return "unknown";
    char buffer[64];
    while (fgets(buffer, sizeof buffer, pipe) != nullptr) result += buffer;
    pclose(pipe);

    // trim newline
    result.erase(result.find_last_not_of(" \n\r\t")+1);
    if (result == "x86_64") return "amd64";
    if (result == "aarch64") return "arm64";
    if (result == "armv7l") return "armhf";
    return result;
}

std::vector<std::string> get_remotes() {
    std::vector<std::string> urls;
    std::string arch = detect_arch();

    fs::path root = "/bit/Chocobitpup/remotes";
    if (!fs::exists(root)) return urls;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().filename() != "remote.choco.list") continue;

        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string type, base, pool;
            iss >> type >> base >> pool;

            if (type != "choco" || base.empty() || pool.empty()) continue;

            std::string channel;
            while (iss >> channel) {
                std::string full_url = base + "/pool/" + pool + "/" + arch + "/" + channel;
                urls.push_back(full_url);
            }
        }
    }
    return urls;
}

Package fetch_package(const std::string& pkgname, const std::string& remote) {
    std::string pointer_url = remote + "/" + pkgname + ".choco.yml";
    std::string pointer_cmd = "curl -s " + pointer_url;
    std::string pointer_data;
    char buffer[128];
    FILE* pipe = popen(pointer_cmd.c_str(), "r");
    while (fgets(buffer, sizeof buffer, pipe) != nullptr) pointer_data += buffer;
    pclose(pipe);

    YAML::Node pointer = YAML::Load(pointer_data);
    std::string thread_url = pointer["url"].as<std::string>();

    std::string thread_cmd = "curl -s " + thread_url;
    std::string thread_data;
    pipe = popen(thread_cmd.c_str(), "r");
    while (fgets(buffer, sizeof buffer, pipe) != nullptr) thread_data += buffer;
    pclose(pipe);

    YAML::Node thread = YAML::Load(thread_data);

    Package pkg;
    pkg.name = thread["name"].as<std::string>();
    pkg.version = thread["version"].as<std::string>();
    pkg.commands = thread["install"]["commands"].as<std::string>();
    pkg.url = thread["source"]["package"].as<std::string>();
    pkg.root = pkgname;

    if (thread["dependencies"]) {
        for (auto dep : thread["dependencies"]) {
            pkg.dependencies.push_back(dep.as<std::string>());
        }
    }
    return pkg;
}

void install_package(const Package& pkg, std::set<std::string>& installed, bool autoYes);

void install_with_deps(const Package& pkg, std::set<std::string>& installed, bool autoYes) {
    for (const auto& dep : pkg.dependencies) {
        if (installed.count(dep)) continue;
        for (const auto& remote : get_remotes()) {
            Package dep_pkg = fetch_package(dep, remote);
            dep_pkg.root = dep;
            install_with_deps(dep_pkg, installed, autoYes);
            break;
        }
    }
    install_package(pkg, installed, autoYes);
}

void save_dependency_record(const std::string& dep, const std::string& owner) {
    fs::path path = "/bit/Chocolaterie/" + dep + "/dependency.json";
    json j;
    if (fs::exists(path)) {
        std::ifstream in(path);
        in >> j;
    }
    j["owners"].push_back(owner);
    std::ofstream out(path);
    out << j.dump(2);
}

void install_package(const Package& pkg, std::set<std::string>& installed, bool autoYes) {
    fs::path path = fs::path("/bit/Chocolaterie") / pkg.root;
    if (fs::exists(path)) return;

    std::cout << "\n\U0001F4E5 Installing:\n- " << pkg.root << "\n";
    if (!pkg.dependencies.empty()) {
        std::cout << "\U0001F4DA Dependencies:\n";
        for (const auto& dep : pkg.dependencies) {
            std::cout << "- " << dep << "\n";
        }
    }
    if (!autoYes) {
        std::string input;
        std::cout << "\u2753 Continue? [Y/n] ";
        std::getline(std::cin, input);
        if (input == "n" || input == "N") {
            std::cout << "Aborted.\n";
            return;
        }
    }

    fs::create_directories(path);
    std::string file = path.string() + "/" + pkg.root + "-" + pkg.version + ".choco.pkg";

    std::cout << "==> 🐶 Downloading " << pkg.url << " ...\n";
    std::string cmd = "curl --progress-bar -L -o \"" + file + "\" \"" + pkg.url + "\"";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "    ⚠️ Download failed for " << pkg.root << "\n";
        return;
    }

    fs::path tmpdir = "/tmp/bitpuppy-extract-" + pkg.root;
    if (fs::exists(tmpdir)) fs::remove_all(tmpdir);
    fs::create_directories(tmpdir);

    std::string tar_cmd = "tar --strip-components=1 -xf \"" + file + "\" -C \"" + tmpdir.string() + "\"";
    std::system(tar_cmd.c_str());

    for (const auto& entry : fs::directory_iterator(tmpdir)) {
        fs::rename(entry.path(), path / entry.path().filename());
    }

    fs::remove_all(tmpdir);
    std::remove(file.c_str());

    std::string run_cmd = pkg.commands;
    size_t pos;
    while ((pos = run_cmd.find("$ROOT")) != std::string::npos) {
        run_cmd.replace(pos, 5, (root == "/" ? "" : root));
    }
    std::system(run_cmd.c_str());
    for (const auto& dep : pkg.dependencies) save_dependency_record(dep, pkg.name);

    std::cout << "    \U0001F36B  " << pkg.root << ": installed v" << pkg.version << "\n";
    installed.insert(pkg.name);
}

void remove_package(const std::string& name, bool autoYes) {
    fs::path path = "/bit/Chocolaterie/" + name;
    if (!fs::exists(path)) {
        std::cerr << "\u2717 Package not found: " << name << "\n";
        return;
    }
    std::cout << "\n\U0001F5D1 Removing:\n- " << name << "\n";
    fs::path depfile = path / "dependency.json";
    if (fs::exists(depfile)) {
        std::ifstream in(depfile);
        json j; in >> j;
        if (j["owners"].empty()) {
            std::cout << "\U0001F4DA Dependencies:\n- " << name << "\n";
        }
    }
    if (!autoYes) {
        std::string input;
        std::cout << "\u2753 Continue? [Y/n] ";
        std::getline(std::cin, input);
        if (input == "n" || input == "N") {
            std::cout << "Aborted.\n";
            return;
        }
    }
    fs::remove_all(path);
    std::cout << "    \u2705 Removed " << name << "\n";
}

void update_all() {
    for (const auto& dir : fs::directory_iterator("/bit/Chocolaterie")) {
        std::string name = dir.path().filename().string();
        std::cout << "    ⬆️  Updating " << name << "...\n";
        for (const auto& remote : get_remotes()) {
            Package pkg = fetch_package(name, remote);
            pkg.root = name;
            std::set<std::string> installed;
            install_package(pkg, installed, true);
            break;
        }
    }
}

void collect_packages_with_deps(const Package& pkg, std::set<std::string>& collected, std::vector<Package>& ordered, const std::vector<std::string>& remotes) {
    if (collected.count(pkg.root)) return;
    collected.insert(pkg.root);
    for (const auto& dep : pkg.dependencies) {
        for (const auto& remote : remotes) {
            Package dep_pkg = fetch_package(dep, remote);
            dep_pkg.root = dep;
            collect_packages_with_deps(dep_pkg, collected, ordered, remotes);
            break;
        }
    }
    ordered.push_back(pkg);
}

int main(int argc, char* argv[]) {
    if (fs::exists("/bit/lock")) {
        if (argc < 2 || std::string(argv[1]) != "unlock") {
            std::cerr << "\u26D4 BitPuppy is locked. Run 'bitpup unlock' to unlock.\n";
            return 1;
        }
    }

    if (argc < 2) {
        std::cout << "\U0001F436 Run 'bitpup help' for help!\n";
        return 0;
    }

    std::string cmd = argv[1];
    bool autoYes = false;
    std::string root = "/";  // Default root
    std::vector<std::string> packages;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-y") autoYes = true;
        else if (arg.rfind("--root=", 0) == 0) root = arg.substr(7);
        else packages.push_back(arg);
    }

    if (cmd == "help") {
        prompt_help();
    } else if (cmd == "remote-add" && !packages.empty()) {
        std::string url = packages[0];
        std::string name = "default";
        std::vector<std::string> channels;
        if (packages.size() >= 2) name = packages[1];
        if (packages.size() > 2) channels.assign(packages.begin() + 2, packages.end());
        add_remote(url, name, channels);
    } else if (cmd == "remove" && !packages.empty()) {
        for (const auto& p : packages) remove_package(p, autoYes);
    } else if (cmd == "install" && !packages.empty()) {
        std::set<std::string> collected_roots;
        std::vector<Package> ordered_packages;
        std::vector<std::string> remotes = get_remotes();

        for (const auto& pkgname : packages) {
            for (const auto& remote : remotes) {
                Package pkg = fetch_package(pkgname, remote);
                pkg.root = pkgname;
                collect_packages_with_deps(pkg, collected_roots, ordered_packages, remotes);
                break;
            }
        }

        std::cout << "\n\U0001F4E5 Installing:\n";
        for (const auto& pkg : ordered_packages) {
            std::cout << "- " << pkg.root << "\n";
        }
        if (!autoYes) {
            std::string input;
            std::cout << "\u2753 Continue? [Y/n] ";
            std::getline(std::cin, input);
            if (input == "n" || input == "N") {
                std::cout << "Aborted.\n";
                return 0;
            }
        }

        std::set<std::string> installed;
        for (const auto& pkg : ordered_packages) {
            install_package(pkg, installed, true);
        }
    } else if (cmd == "update") {
        update_all();
    } else if (cmd == "lock") {
        std::ofstream lockfile("/opt/bitpuppy/lock");
        lockfile << "locked\n";
        lockfile.close();
        std::cout << "\U0001F512 BitPuppy locked.\n";
    } else if (cmd == "unlock") {
        if (fs::exists("/opt/bitpuppy/lock")) {
            fs::remove("/opt/bitpuppy/lock");
            std::cout << "\U0001F513 BitPuppy unlocked.\n";
        } else {
            std::cout << "BitPuppy was not locked.\n";
        }
    } else if (cmd == "version") {
        std::cerr << "🍫 BitPuppy 3.1.1 \n";
    } else {
        std::cerr << "\u274C Error: '" << cmd << "' is not a valid option.\n";
        std::cout << "\u2753 Maybe you meant 'install'?\n";
        std::cout << "\U0001F436 Run 'bitpup help' for help!\n";
    }

    return 0;
}
