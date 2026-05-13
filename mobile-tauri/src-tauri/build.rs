use std::env;
use std::path::PathBuf;

fn main() {
    // Build script: Link to libdinero_wallet_ffi.a
    // This library is built by CMake in the parent DineroCoin project
    
    // Find libdinero_wallet_ffi.a in build directory
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let project_root = PathBuf::from(&manifest_dir)
        .parent()
        .unwrap()
        .parent()
        .unwrap();
    
    // Look for built library in various build directories
    let possible_lib_paths = [
        project_root.join("build").join("libdinero_wallet_ffi.a"),
        project_root.join("build-release").join("libdinero_wallet_ffi.a"),
        project_root.join("build-mobile").join("libdinero_wallet_ffi.a"),
        project_root.join("..").join("build").join("libdinero_wallet_ffi.a"),
        project_root.join("..").join("build-release").join("libdinero_wallet_ffi.a"),
    ];
    
    let mut lib_found = false;
    for lib_path in &possible_lib_paths {
        if lib_path.exists() {
            println!("cargo:rustc-link-search=native={}", lib_path.parent().unwrap().display());
            println!("cargo:rustc-link-lib=static=dinero_wallet_ffi");
            lib_found = true;
            
            // Also link dependencies (dinero_wallet_ffi depends on these)
            println!("cargo:rustc-link-lib=static=dinero_wallet");
            println!("cargo:rustc-link-lib=static=dinero_crypto");
            println!("cargo:rustc-link-lib=static=dinero_consensus");
            println!("cargo:rustc-link-lib=static=secp256k1");
            println!("cargo:rustc-link-lib=static=jsoncpp");
            println!("cargo:rustc-link-lib=static=sqlite3");
            println!("cargo:rustc-link-lib=c++");
            
            // Link system libraries
            if cfg!(target_os = "macos") {
                println!("cargo:rustc-link-lib=framework=Security");
                println!("cargo:rustc-link-lib=framework=Foundation");
            } else if cfg!(target_os = "linux") {
                println!("cargo:rustc-link-lib=dl");
                println!("cargo:rustc-link-lib=pthread");
            }
            
            break;
        }
    }
    
    if !lib_found {
        println!("cargo:warning=libdinero_wallet_ffi.a not found. Building wallet core first:");
        println!("cargo:warning=  cd DineroCoin && cmake --build build --target dinero_wallet_ffi");
    }
    
    // Tell Cargo to rerun if build script or FFI header changes
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=../../wallet-core/ffi/wallet_ffi.h");
}

