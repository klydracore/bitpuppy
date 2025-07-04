// Updated Bitey with new prompts, dependency support, and multi-package install

use clap::{Parser, Subcommand};
use serde::{Deserialize, Serialize};
use std::{collections::{HashMap, HashSet}, fs, path::PathBuf, process::Command};
use walkdir::WalkDir;
use dialoguer::Confirm;

#[derive(Parser)]
#[command(name = "bitey")]
#[command(about = "The Bitey Package Manager", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    Install {
        packages: Vec<String>,
        #[arg(short, long)]
        yes: bool,
        #[arg(long)]
        insecure: bool,
    },
    Remove {
        package: String,
        #[arg(short, long)]
        yes: bool,
    },
    RemoteAdd {
        remote: String,
    },
    Update {
        packages: Vec<String>,
        #[arg(long)]
        insecure: bool,
    },
    Help,
}

#[derive(Debug, Serialize, Deserialize)]
struct RemoteYml { url: String }

#[derive(Debug, Serialize, Deserialize)]
struct PackageYml {
    name: String,
    version: String,
    maintainer: String,
    description: String,
    source: SourceUrls,
    install: InstallBlock,
    dependencies: Option<Vec<String>>,
}

#[derive(Debug, Serialize, Deserialize)]
struct SourceUrls {
    raw: Option<String>,
    package: Option<String>,
}

#[derive(Debug, Serialize, Deserialize)]
struct InstallBlock {
    commands: String,
}

fn prompt_confirm(message: &str, auto_yes: bool) -> bool {
    if auto_yes { return true; }
    Confirm::new().with_prompt(message).default(true).interact().unwrap()
}

fn find_remotes(remotes_dir: &str) -> HashMap<String, String> {
    let mut remotes = HashMap::new();
    for entry in WalkDir::new(remotes_dir).into_iter().filter_map(Result::ok).filter(|e| e.file_name() == "remote.yml") {
        let contents = fs::read_to_string(entry.path()).unwrap();
        let remote: RemoteYml = serde_yaml::from_str(&contents).unwrap();
        let name = entry.path().parent().unwrap().file_name().unwrap().to_string_lossy().to_string();
        remotes.insert(name, remote.url);
    }
    remotes
}

fn fetch_thread(remote_url: &str, package_name: &str, insecure: bool) -> (String, PackageYml) {
    let pointer_url = format!("{}/{}.yml", remote_url, package_name);
    let client = reqwest::blocking::Client::builder().danger_accept_invalid_certs(insecure).build().unwrap();
    let pointer_content = client.get(&pointer_url).send().unwrap().text().unwrap();
    let pointer: serde_yaml::Value = serde_yaml::from_str(&pointer_content).unwrap();
    let thread_url = pointer["url"].as_str().unwrap();
    let thread_content = client.get(thread_url).send().unwrap().text().unwrap();
    let pkg: PackageYml = serde_yaml::from_str(&thread_content).unwrap();
    (pointer_content, pkg)
}

fn install_with_deps(packages: Vec<String>, auto_yes: bool, insecure: bool) {
    let remotes = find_remotes(...);
    let mut all_to_install = HashMap::new();
    let mut seen = HashSet::new();

    for pkg in packages {
        for (_, remote_url) in &remotes {
            let list = fetch_package_list(remote_url, insecure);
            if list.contains(&pkg) {
                let (ptr, meta) = fetch_thread(remote_url, &pkg, insecure);
                collect_deps(&meta, remote_url, &mut all_to_install, &mut seen, insecure);
                all_to_install.insert(pkg.clone(), (meta, ptr));
                break;
            }
        }
    }

    println!("\n📥 Installing:");
    for key in all_to_install.keys() {
        println!("- {}", key);
    }
    println!("\n❓ Continue? [Y/n]");
    if !prompt_confirm("Install the listed packages?", auto_yes) {
        println!("Aborted.");
        return;
    }

    for (name, (pkg, ptr)) in all_to_install {
        install_package(&pkg, &name, insecure, &ptr);
    }
}

fn collect_deps(pkg: &PackageYml, remote_url: &str, all: &mut HashMap<String, (PackageYml, String)>, seen: &mut HashSet<String>, insecure: bool) {
    if let Some(deps) = &pkg.dependencies {
        for dep in deps {
            if seen.contains(dep) { continue; }
            seen.insert(dep.clone());
            let (ptr, meta) = fetch_thread(remote_url, dep, insecure);
            collect_deps(&meta, remote_url, all, seen, insecure);
            all.insert(dep.clone(), (meta, ptr));
        }
    }
}

fn fetch_package_list(remote_url: &str, insecure: bool) -> Vec<String> {
    let list_url = format!("{}/list.txt", remote_url);
    let client = reqwest::blocking::Client::builder().danger_accept_invalid_certs(insecure).build().unwrap();
    let response = client.get(&list_url).send().unwrap().text().unwrap();
    response.lines().map(|s| s.to_string()).collect()
}

fn install_package(pkg: &PackageYml, name: &str, insecure: bool, pointer_yaml: &str) {
    let install_dir = format!("/opt/bitey/Chocolaterie/{}", name);
    fs::create_dir_all(&install_dir).unwrap();
    fs::write(format!("{}/Thread.yml", install_dir), serde_yaml::to_string(pkg).unwrap()).unwrap();
    fs::write(format!("{}/package.yml", install_dir), pointer_yaml).unwrap();

    if let Some(url) = &pkg.source.package {
        let out_file = format!("{}/{}.choco.pkg", install_dir, name);
        let mut args = vec!["--progress-bar", "-L", "-o", &out_file, url];
        if insecure { args.insert(0, "--insecure"); }
        Command::new("curl").args(&args).status().unwrap();
        Command::new("tar").args(["-xf", &out_file, "-C", &install_dir]).status().unwrap();
        let _ = fs::remove_file(&out_file);
    }

    Command::new("sh").arg("-c").arg(&pkg.install.commands).status().unwrap();
    println!("🍫 {}: installed v{}", pkg.name, pkg.version);
}

fn remove_package(name: &str, auto_yes: bool) {
    let dir = format!("/opt/bitey/Chocolaterie/{}", name);
    if !PathBuf::from(&dir).exists() {
        eprintln!("✗ Package not found: {}", name);
        return;
    }
    println!("\n🗑 Removing:\n- {}", name);
    if !prompt_confirm("❓ Continue?", auto_yes) {
        println!("Aborted.");
        return;
    }
    Command::new("rm").args(["-rf", &dir]).status().unwrap();
    println!("✅ Removed {}", name);
}

fn add_remote(arg: &str) {
    let url = if arg.starts_with("ppa:") {
        let path = arg.trim_start_matches("ppa:");
        format!("http://ppa.wheedev.org/{}", path)
    } else { arg.to_string() };

    let name = url.replace("http://", "").replace("https://", "").replace("/", "_");
    let dir = format!("/opt/bitey/Chocobitey/remotes/{}", name);
    fs::create_dir_all(&dir).unwrap();
    fs::write(format!("{}/remote.yml", dir), format!("url: {}\n", url)).unwrap();
    println!("✅ Remote added: {}", url);
}

fn main() {
    let cli = Cli::parse();
    match &cli.command {
        Commands::Install { packages, yes, insecure } => {
            if packages.is_empty() {
                println!("🐶 Run 'bitey help' for help!");
            } else {
                install_with_deps(packages.clone(), *yes, *insecure);
            }
        },
        Commands::Remove { package, yes } => remove_package(package, *yes),
        Commands::RemoteAdd { remote } => add_remote(remote),
        Commands::Update { packages, insecure } => {
            let pkgs = if packages.is_empty() {
                fs::read_dir("/opt/bitey/Chocolaterie").unwrap().filter_map(Result::ok).map(|e| e.file_name().into_string().unwrap()).collect()
            } else { packages.clone() };
            for pkg in pkgs { update_package(&pkg, *insecure); }
        },
        Commands::Help => {
            println!("\n❓ Help:\n\n📦 Packages:\n- install <package> - Install a package.\n- remove <package> - Remove a package.\n- update - Update all packages.\n\n🌐 Remotes:\n- remote-add <url> - Add a remote from URL.\n- remote-add ppa:<profile>/<ppa> - Add a PPA.\n");
        }
    }
}

fn update_package(name: &str, insecure: bool) {
    let dir = format!("/opt/bitey/Chocolaterie/{}", name);
    let pkg_yml = format!("{}/package.yml", dir);
    let thread_yml = format!("{}/Thread.yml", dir);
    if !PathBuf::from(&pkg_yml).exists() {
        println!("✗ Package {} not installed", name);
        return;
    }
    let pointer_str = fs::read_to_string(&pkg_yml).unwrap();
    let pointer: serde_yaml::Value = serde_yaml::from_str(&pointer_str).unwrap();
    let url = pointer["url"].as_str().unwrap();
    let client = reqwest::blocking::Client::builder().danger_accept_invalid_certs(insecure).build().unwrap();
    let new_thread_str = client.get(url).send().unwrap().text().unwrap();
    let new_pkg: PackageYml = serde_yaml::from_str(&new_thread_str).unwrap();
    let local_pkg: Option<PackageYml> = fs::read_to_string(&thread_yml).ok().and_then(|s| serde_yaml::from_str(&s).ok());
    let needs_update = local_pkg.map_or(true, |l| l.version != new_pkg.version);
    if needs_update {
        println!("⬆️   Updating {} → v{}", name, new_pkg.version);
        install_package(&new_pkg, name, insecure, &pointer_str);
    } else {
        println!("✔ {} is up to date (v{})", name, new_pkg.version);
    }
}
