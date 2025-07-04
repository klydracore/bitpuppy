// Bitey C++ CLI full version (install, remove, remote-add, update, deps)

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

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Package {
    std::string name;
    std::vector<std::string> dependencies;
    std::string version;
    std::string commands;
    std::string url;
    std::string root; // added
};

void prompt_help() {
    std::cout << "\n\u2753 Help:\n\n\U0001F4E6 Packages:\n"
              << "- install <package> - Install a package.\n"
              << "- remove <package> - Remove a package.\n"
              << "- update - Update all packages.\n\n"
              << "\U0001F310 Remotes:\n"
              << "- remote-add <url> - Add a remote from URL.\n"
              << "- remote-add ppa:<profile>/<ppa> - Add a PPA.\n\n";
}

void add_remote(const std::string& urlArg) {
    std::string url = urlArg;
    if (url.starts_with("ppa:")) {
        url = "http://ppa.wheedev.org/" + url.substr(4);
    }

    std::string name = url;
    for (char& c : name) if (c == '/' || c == ':') c = '_';
    fs::path dir = "/opt/bitey/Chocobitey/remotes/" + name;
    fs::create_directories(dir);

    std::ofstream file(dir / "remote.yml");
    file << "url: " << url << "\n";
    file.close();

    std::cout << "\u2705 Remote added: " << url << "\n";
}

std::vector<std::string> get_remotes() {
    std::vector<std::string> urls;
    for (const auto& entry : fs::directory_iterator("/opt/bitey/Chocobitey/remotes")) {
        std::ifstream in(entry.path() / "remote.yml");
        YAML::Node node = YAML::Load(in);
        urls.push_back(node["url"].as<std::string>());
    }
    return urls;
}

Package fetch_package(const std::string& pkgname, const std::string& remote) {
    std::string pointer_url = remote + "/" + pkgname + ".yml";
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
    pkg.root = pkgname; // <<<< use original YAML name here

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
            dep_pkg.root = pkg.root; // <- propagate the root
            install_with_deps(dep_pkg, installed, autoYes);
            break;
        }
    }
    install_package(pkg, installed, autoYes);
}

void save_dependency_record(const std::string& dep, const std::string& owner) {
    fs::path path = "/opt/bitey/Chocolaterie/" + dep + "/dependency.json";
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
    fs::path path = "/opt/bitey/Chocolaterie/" + pkg.root;
    if (fs::exists(path)) return;

    std::cout << "\n\U0001F4E5 Installing:\n- " << pkg.name << "\n";
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
    std::string file = path.string() + "/" + pkg.name + ".choco.pkg";
    std::string cmd = "curl -s -L -o " + file + " " + pkg.url;
    std::system(cmd.c_str());

    std::string tar_cmd = "tar -xf " + file + " -C " + path.string();
    std::system(tar_cmd.c_str());
    std::remove(file.c_str());

    std::system(pkg.commands.c_str());
    for (const auto& dep : pkg.dependencies) save_dependency_record(dep, pkg.name);

    std::cout << "\U0001F36B  " << pkg.name << ": installed v" << pkg.version << "\n";
    installed.insert(pkg.name);
}

void remove_package(const std::string& name, bool autoYes) {
    fs::path path = "/opt/bitey/Chocolaterie/" + name;
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
    std::cout << "\u2705 Removed " << name << "\n";
}

void update_all() {
    for (const auto& dir : fs::directory_iterator("/opt/bitey/Chocolaterie")) {
        std::string name = dir.path().filename().string();
        std::cout << "⬆️  Updating " << name << "...\n";
        for (const auto& remote : get_remotes()) {
            Package pkg = fetch_package(name, remote);
            pkg.root = name;
            std::set<std::string> installed;
            install_package(pkg, installed, true);
            break;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "\U0001F436 Run 'bitey help' for help!\n";
        return 0;
    }
    std::string cmd = argv[1];
    bool autoYes = false;
    std::vector<std::string> packages;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-y") autoYes = true;
        else packages.push_back(arg);
    }

    if (cmd == "help") {
        prompt_help();
    } else if (cmd == "remote-add" && !packages.empty()) {
        add_remote(packages[0]);
    } else if (cmd == "remove" && !packages.empty()) {
        for (const auto& p : packages) remove_package(p, autoYes);
    } else if (cmd == "install" && !packages.empty()) {
        std::set<std::string> installed;
        for (const auto& pkgname : packages) {
            for (const auto& remote : get_remotes()) {
                Package pkg = fetch_package(pkgname, remote);
                pkg.root = pkgname;
                install_with_deps(pkg, installed, autoYes);
                break;
            }
        }
    } else if (cmd == "update") {
        update_all();
    } else {
        std::cerr << "\u274C Error: '" << cmd << "' is not a valid option.\n";
        std::cout << "\u2753 Maybe you meant 'install'?\n";
        std::cout << "\U0001F436 Run 'bitey help' for help!\n";
    }
    return 0;
}
