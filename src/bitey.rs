use clap::{Parser, Subcommand};
use serde::Deserialize;
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
        /// Package to install
        package: String,

        /// Skip SSL cert verification
        #[arg(long)]
        insecure: bool,
    },
}

#[derive(Debug, Deserialize)]
struct RemoteYml {
    url: String,
}

#[derive(Debug, Deserialize)]
struct PackageYml {
    name: String,
    version: String,
    maintainer: String,
    description: String,
    source: SourceUrls,
    install: InstallBlock,
}

#[derive(Debug, Deserialize)]
struct SourceUrls {
    raw: Option<String>,
    package: Option<String>,
}

#[derive(Debug, Deserialize)]
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

fn fetch_package_yml(remote_url: &str, package_name: &str, insecure: bool) -> PackageYml {
    let url = format!("{}/{}.yml", remote_url, package_name);
    let client = reqwest::blocking::Client::builder()
        .danger_accept_invalid_certs(insecure)
        .build()
        .expect("HTTP client build failed");

    let response = client
        .get(&url)
        .send()
        .expect("Failed to fetch package.yml")
        .text()
        .expect("Failed to read package.yml");

    serde_yaml::from_str(&response).expect("Invalid package.yml format")
}

fn install_package(pkg: &PackageYml, package_name: &str, insecure: bool) {
    let install_dir = format!("/opt/bitey/Chocolaterie/{}", package_name);
    fs::create_dir_all(&install_dir).expect("Failed to create package install dir");

    // Download .choco.pkg
    if let Some(pkg_url) = &pkg.source.package {
        let filename = format!("{}/{}.choco.pkg", install_dir, package_name);
        let client = reqwest::blocking::Client::builder()
            .danger_accept_invalid_certs(insecure)
            .build()
            .expect("HTTP client build failed");

        let response = client
            .get(pkg_url)
            .send()
            .expect("Failed to download package")
            .bytes()
            .expect("Invalid package content");

        fs::write(&filename, &response).expect("Failed to write package file");
    }

    // Run install
    println!("Installing {} v{}...", pkg.name, pkg.version);
    Command::new("sh")
        .arg("-c")
        .arg(&pkg.install.commands)
        .status()
        .expect("Install script failed");
}

fn main() {
    let cli = Cli::parse();

    match &cli.command {
        Commands::Install { package, insecure } => {
            let remotes = find_remotes("/opt/bitey/Chocobitey/remotes");

            for (remote_name, remote_url) in remotes {
                println!("Fetching from remote: {} ({})", remote_name, remote_url);
                let packages = fetch_package_list(&remote_url, *insecure);

                if packages.contains(package) {
                    let pkg = fetch_package_yml(&remote_url, package, *insecure);
                    install_package(&pkg, package, *insecure);
                    return;
                }
            }

            eprintln!("✗ Package not found: {}", package);
            std::process::exit(1);
        }
    }
}
