{
  description = "cagent: high performance C AI coding agent runtime";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              # Compiler / tooling
              clang
              clang-tools

              # Build system
              cmake
              ninja
              pkg-config

              # Runtime dependencies
              curl
              openssl
              yyjson
              ncurses

              # Debug / analysis
              gdb
              valgrind

              # Development utilities
              git
              ripgrep
            ];

            shellHook = ''
              # Unified clang toolchain for local development: clang, clangd,
              # clang-tidy, clang-format all see the same headers/flags.
              # nix build (packages.default) is unaffected; it uses stdenv.
              export CC=clang
              export CXX=clang++

              # clang-tidy (from clang-tools) lacks the cc-wrapper's injected
              # glibc include path, so hand it over explicitly.
              if [ -z "$C_INCLUDE_PATH" ]; then
                export C_INCLUDE_PATH="${pkgs.glibc.dev}/include"
              else
                export C_INCLUDE_PATH="${pkgs.glibc.dev}/include:$C_INCLUDE_PATH"
              fi

              echo "cagent development environment"
              echo "Compiler: $(clang --version | head -n1)"
              echo "CMake:    $(cmake --version | head -n1)"
              echo "yyjson:   $(find ${pkgs.yyjson}/include -name yyjson.h | head -n1)"
            '';
          };
        });

      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "cagent";
            version = "0.1.0";
            src = self;

            nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
            buildInputs = with pkgs; [ curl openssl yyjson ncurses ];

            cmakeBuildType = "Release";

            meta = {
              description = "High performance C AI coding agent runtime";
              license = pkgs.lib.licenses.mit;
              platforms = pkgs.lib.platforms.linux;
            };
          };
        });

      checks = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          build = self.packages.${system}.default;
        });
    };
}
