{
  description = "LeanStore - high-performance OLTP storage engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # Pre-fetch the three ExternalProject_Add dependencies at the exact
        # commits pinned in libs/*.cmake so sandbox builds work without network.
        # Run `nix build` once and paste the correct hashes from the error output.
        gflags-src = pkgs.fetchFromGitHub {
          owner = "gflags";
          repo = "gflags";
          rev = "f8a0efe03aa69b3336d8e228b37d4ccb17324b88";
          hash = "sha256-lydtiYXoVeT6T6pDubFy4goI/mdkBGfD5hJi5CcCqPg=";
        };

        rapidjson-src = pkgs.fetchFromGitHub {
          owner = "Tencent";
          repo = "rapidjson";
          rev = "1c2c8e085a8b2561dff17bedb689d2eb0609b689";
          hash = "sha256-aYurT2JFh8Rp3Gm4o+A5uXN6rmFez5q8XvNbIC5DxoA=";
        };

        tabulate-src = pkgs.fetchFromGitHub {
          owner = "p-ranav";
          repo = "tabulate";
          rev = "718d827cf05c2e9bba17e926cac2d7ab2356621c";
          hash = "sha256-MSUTunp4RjBy7DuZUQ00GYzdUTjG1CIl7AJMikXlTq4=";
        };

        # Rewritten cmake module files that point to pre-fetched Nix store paths
        # instead of fetching from git at configure time.
        gflags-cmake = pkgs.writeText "gflags.cmake" ''
          include(ExternalProject)
          ExternalProject_Add(
            gflags_src
            PREFIX "vendor/gflags"
            SOURCE_DIR "${gflags-src}"
            DOWNLOAD_COMMAND ""
            UPDATE_COMMAND ""
            CMAKE_ARGS
              -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
              -DCMAKE_C_COMPILER=''${CMAKE_C_COMPILER}
              -DCMAKE_CXX_COMPILER=''${CMAKE_CXX_COMPILER}
              -DCMAKE_CXX_FLAGS=''${CMAKE_CXX_FLAGS}
          )
          ExternalProject_Get_Property(gflags_src install_dir)
          set(GFLAGS_INCLUDE_DIR ''${install_dir}/include)
          set(GFLAGS_LIBRARY_PATH ''${install_dir}/lib/libgflags.a)
          file(MAKE_DIRECTORY ''${GFLAGS_INCLUDE_DIR})
          add_library(gflags STATIC IMPORTED)
          set_property(TARGET gflags PROPERTY IMPORTED_LOCATION ''${GFLAGS_LIBRARY_PATH})
          set_property(TARGET gflags APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ''${GFLAGS_INCLUDE_DIR})
          add_dependencies(gflags gflags_src)
        '';

        rapidjson-cmake = pkgs.writeText "rapidjson.cmake" ''
          include(ExternalProject)
          ExternalProject_Add(
            rapidjson_src
            PREFIX "vendor/rapidjson"
            SOURCE_DIR "${rapidjson-src}"
            DOWNLOAD_COMMAND ""
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND ""
            UPDATE_COMMAND ""
          )
          ExternalProject_Get_Property(rapidjson_src source_dir)
          set(RAPIDJSON_INCLUDE_DIR ''${source_dir}/include)
          file(MAKE_DIRECTORY ''${RAPIDJSON_INCLUDE_DIR})
          add_library(rapidjson INTERFACE)
          set_property(TARGET rapidjson APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ''${RAPIDJSON_INCLUDE_DIR})
          add_dependencies(rapidjson rapidjson_src)
        '';

        tabulate-cmake = pkgs.writeText "tabluate.cmake" ''
          include(ExternalProject)
          ExternalProject_Add(
            tabluate_src
            PREFIX "vendor/tabluate"
            SOURCE_DIR "${tabulate-src}"
            DOWNLOAD_COMMAND ""
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND ""
            UPDATE_COMMAND ""
          )
          ExternalProject_Get_Property(tabluate_src source_dir)
          set(TABLUATE_INCLUDE_DIR ''${source_dir}/include)
          file(MAKE_DIRECTORY ''${TABLUATE_INCLUDE_DIR})
          add_library(tabluate INTERFACE IMPORTED)
          set_property(TARGET tabluate APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ''${TABLUATE_INCLUDE_DIR})
          add_dependencies(tabluate tabluate_src)
        '';

      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "leanstore";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            cmake
            git
          ];

          buildInputs = with pkgs; [
            tbb
            libaio
            rocksdb
            lmdb
            wiredtiger
            zlib
            bzip2
            lz4
            snappy
            zstd
            liburing
          ];

          prePatch = ''
            cp ${gflags-cmake} libs/gflags.cmake
            cp ${rapidjson-cmake} libs/rapidjson.cmake
            cp ${tabulate-cmake} libs/tabluate.cmake
          '';

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
          ];

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            for bin in frontend/frontend frontend/ycsb frontend/deterministic \
                       frontend/tpcc frontend/bulk_loading frontend/graveyard \
                       frontend/queue frontend/minimal_example frontend/scaled_example \
                       frontend/rocksdb_tpcc frontend/rocksdb_ycsb \
                       frontend/wiredtiger_tpcc frontend/wiredtiger_ycsb \
                       frontend/lmdb_tpcc frontend/lmdb_ycsb; do
              [ -f "$bin" ] && cp "$bin" $out/bin/
            done
            runHook postInstall
          '';
        };

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            git
            tbb
            libaio
            rocksdb
            lmdb
            wiredtiger
            zlib
            bzip2
            lz4
            snappy
            zstd
            liburing
            libnvme
            boost
            pkg-config
          ];

          shellHook = ''
            # gflags/tabulate declare cmake_minimum_required < 3.5, which the
            # CMake 4.x in nixpkgs refuses. CMake reads this env var and forwards
            # it into the ExternalProject sub-builds, so no upstream files change.
            export CMAKE_POLICY_VERSION_MINIMUM=3.5
            echo "LeanStore dev environment"
            echo "Build: mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. && make -j\$(nproc)"
          '';
        };
      }
    );
}
