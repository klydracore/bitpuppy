use clap::{Parser, Subcommand};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, fs, path::PathBuf, process::Command};
use walkdir::WalkDir;

/// CLI tool for Bitey
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
        package: String,
        #[arg(long)]
        insecure: bool,
    },
    Remove {
        package: String,
    },
    RemoteAdd {
        remote: String,
    },
    Update {
        packages: Vec<String>,
        #[arg(long)]
        insecure: bool,
    },
}

#[derive(Debug, Serialize, Deserialize)]
struct RemoteYml {
    url: String,
}

#[derive(Debug, Serialize, Deserialize)]
struct PackageYml {
    name: String,
    version: String,
    maintainer: String,
    description: String,
    source: SourceUrls,
    install: InstallBlock,
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

fn find_remotes(remotes_dir: &str) -> HashMap<String, String> {
    let mut remotes = HashMap::new();
    for entry in WalkDir::new(remotes_dir)
        .into_iter()
        .filter_map(Result::ok)
        .filter(|e| e.file_name() == "remote.yml")
    {
        let contents = fs::read_to_string(entry.path()).expect("Failed to read remote.yml");
        let remote: RemoteYml = serde_yaml::from_str(&contents).expect("Invalid YAML format");
        let name = entry
            .path()
            .parent()
            .unwrap()
            .file_name()
            .unwrap()
            .to_string_lossy()
            .to_string();
        remotes.insert(name, remote.url);
    }
    remotes
}

fn fetch_package_list(remote_url: &str, insecure: bool) -> Vec<String> {
    let list_url = format!("{}/list.txt", remote_url);
    let client = reqwest::blocking::Client::builder()
        .danger_accept_invalid_certs(insecure)
        .build()
        .expect("HTTP client build failed");

    let response = client
        .get(&list_url)
        .send()
        .expect("Failed to fetch list.txt")
        .text()
        .expect("Failed to read list.txt");

    response.lines().map(|s| s.to_string()).collect()
}

fn fetch_thread_from_pointer(remote_url: &str, package_name: &str, insecure: bool) -> (String, PackageYml) {
    let pointer_url = format!("{}/{}.yml", remote_url, package_name);
    let client = reqwest::blocking::Client::builder()
        .danger_accept_invalid_certs(insecure)
        .build()
        .expect("HTTP client build failed");

    let pointer_content = client
        .get(&pointer_url)
        .send()
        .expect("Failed to fetch pointer YAML")
        .text()
        .expect("Failed to read pointer YAML");

    #[derive(Deserialize)]
    struct Pointer { url: String }

    let pointer: Pointer = serde_yaml::from_str(&pointer_content).expect("Invalid pointer format");

    let thread_content = client
        .get(&pointer.url)
        .send()
        .expect("Failed to fetch Thread.yml")
        .text()
        .expect("Failed to read Thread.yml");

    let package: PackageYml = serde_yaml::from_str(&thread_content).expect("Invalid Thread.yml format");

    (pointer_content, package)
}

fn install_package(pkg: &PackageYml, package_name: &str, insecure: bool, pointer_yaml: &str) {
    let install_dir = format!("/opt/bitey/Chocolaterie/{}", package_name);
    fs::create_dir_all(&install_dir).expect("Failed to create install directory");

    fs::write(format!("{}/Thread.yml", install_dir), serde_yaml::to_string(pkg).unwrap())
        .expect("Failed to write Thread.yml");
    fs::write(format!("{}/package.yml", install_dir), pointer_yaml)
        .expect("Failed to write package.yml");

    if let Some(pkg_url) = &pkg.source.package {
        let out_file = format!("{}/{}.choco.pkg", install_dir, package_name);
        println!("==> Downloading package: {}", pkg_url);

        let mut curl_args = vec!["--progress-bar", "-L", "-o", &out_file, pkg_url];
        if insecure {
            curl_args.insert(0, "--insecure");
        }

        let status = Command::new("curl")
            .args(curl_args)
            .status()
            .expect("Failed to run curl");

        if !status.success() {
            eprintln!("✗ curl failed to download package");
            std::process::exit(1);
        }

        let status = Command::new("tar")
            .args(["-xf", &out_file, "-C", &install_dir])
            .status()
            .expect("Failed to extract package");

        if !status.success() {
            eprintln!("✗ tar failed to extract package");
            std::process::exit(1);
        }

        let _ = fs::remove_file(&out_file);
    }

    println!("==> Installing {} v{}...", pkg.name, pkg.version);
    let status = Command::new("sh")
        .arg("-c")
        .arg(&pkg.install.commands)
        .status()
        .expect("Install script failed");

    if !status.success() {
        eprintln!("✗ Installation script failed");
        std::process::exit(1);
    }

    println!("🍫  {}: installed v{}", pkg.name, pkg.version);
}

fn update_package(package_name: &str, insecure: bool) {
    let dir = format!("/opt/bitey/Chocolaterie/{}", package_name);
    let pkg_yml = format!("{}/package.yml", dir);
    let thread_yml = format!("{}/Thread.yml", dir);

    if !PathBuf::from(&pkg_yml).exists() {
        println!("✗ Package {} not installed", package_name);
        return;
    }

    let pointer_str = fs::read_to_string(&pkg_yml).expect("Failed to read local package.yml");
    #[derive(Deserialize)]
    struct Pointer { url: String }
    let pointer: Pointer = serde_yaml::from_str(&pointer_str).expect("Invalid package.yml format");

    let client = reqwest::blocking::Client::builder()
        .danger_accept_invalid_certs(insecure)
        .build()
        .expect("HTTP client build failed");

    let new_thread_str = client
        .get(&pointer.url)
        .send()
        .expect("Failed to fetch new Thread.yml")
        .text()
        .expect("Failed to read new Thread.yml");

    let new_pkg: PackageYml = serde_yaml::from_str(&new_thread_str).expect("Invalid Thread.yml");

    let local_pkg: Option<PackageYml> = fs::read_to_string(&thread_yml)
        .ok()
        .and_then(|s| serde_yaml::from_str(&s).ok());

    let needs_update = match &local_pkg {
        Some(local) => local.version != new_pkg.version,
        None => true,
    };

    if needs_update {
        println!("⬆️   Updating {} → v{}", package_name, new_pkg.version);
        install_package(&new_pkg, package_name, insecure, &pointer_str);
    } else {
        println!("✔ {} is up to date (v{})", package_name, new_pkg.version);
    }
}

fn remove_package(package_name: &str) {
    let install_dir = format!("/opt/bitey/Chocolaterie/{}", package_name);

    if !PathBuf::from(&install_dir).exists() {
        eprintln!("✗ Package not found: {}", package_name);
        std::process::exit(1);
    }

    let status = Command::new("rm")
        .args(["-rf", &install_dir])
        .status()
        .expect("Failed to run rm -rf");

    if status.success() {
        println!("🗑️  {} removed successfully", package_name);
    } else {
        eprintln!("✗ Failed to remove package");
        std::process::exit(1);
    }
}

fn add_remote(remote_arg: &str) {
    let url = if remote_arg.starts_with("ppa:") {
        let path = remote_arg.trim_start_matches("ppa:");
        format!("http://ppa.wheedev.org/{}", path)
    } else {
        remote_arg.to_string()
    };

    let name = url
        .replace("http://", "")
        .replace("https://", "")
        .replace("/", "_");

    let target_dir = format!("/opt/bitey/Chocobitey/remotes/{}", name);
    let remote_yml = format!("url: {}\n", url);

    match fs::create_dir_all(&target_dir) {
        Ok(_) => {
            let file_path = format!("{}/remote.yml", target_dir);
            if let Err(e) = fs::write(&file_path, remote_yml) {
                eprintln!("✗ Failed to write remote.yml: {}", e);
                std::process::exit(1);
            }
            println!("✅ Remote added: {}", url);
        }
        Err(e) => {
            eprintln!("✗ Failed to create remote directory: {}", e);
            std::process::exit(1);
        }
    }
}

fn main() {
    let cli = Cli::parse();

    match &cli.command {
        Commands::Install { package, insecure } => {
            let remotes = find_remotes("/opt/bitey/Chocobitey/remotes");
            for (_remote_name, remote_url) in remotes {
                let packages = fetch_package_list(&remote_url, *insecure);
                if packages.contains(package) {
                    let (pointer, pkg) = fetch_thread_from_pointer(&remote_url, package, *insecure);
                    install_package(&pkg, package, *insecure, &pointer);
                    return;
                }
            }
            eprintln!("✗ Package not found: {}", package);
            std::process::exit(1);
        }
        Commands::Remove { package } => remove_package(package),
        Commands::RemoteAdd { remote } => add_remote(remote),
        Commands::Update { packages, insecure } => {
            if packages.is_empty() {
                for entry in fs::read_dir("/opt/bitey/Chocolaterie").unwrap() {
                    let path = entry.unwrap().path();
                    if path.is_dir() {
                        let name = path.file_name().unwrap().to_string_lossy().to_string();
                        update_package(&name, *insecure);
                    }
                }
            } else {
                for pkg in packages {
                    update_package(pkg, *insecure);
                }
            }
        }
    }
}
