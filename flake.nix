{
  description = "Review — a focused PDF and image viewer for GNOME";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in {
      packages = forAllSystems (pkgs:
        let
          review = pkgs.stdenv.mkDerivation {
            pname = "review";
            version = "0.1.0";

            src = pkgs.lib.cleanSourceWith {
              src = ./.;
              filter = path: _type:
                let name = baseNameOf path;
                in !(builtins.elem name [ "build" "build-manual" ".git" ".direnv" ]);
            };

            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              wrapGAppsHook4
            ];

            buildInputs = with pkgs; [
              gtk4
              libadwaita
              poppler
              cairo
              gdk-pixbuf
              glib
              gsettings-desktop-schemas
              adwaita-icon-theme
              hicolor-icon-theme
              shared-mime-info
            ];

            nativeCheckInputs = [ pkgs.qpdf ];

            # Document::open() uses the shared-mime-info database to detect PDFs.
            preCheck = ''
              export XDG_DATA_DIRS="${pkgs.shared-mime-info}/share"
            '';

            # The app shells out to qpdf when saving PDFs and uses the shared
            # MIME database to detect file types.
            preFixup = ''
              gappsWrapperArgs+=(
                --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.qpdf ]}
                --prefix XDG_DATA_DIRS : ${pkgs.shared-mime-info}/share
              )
            '';

            doCheck = true;

            meta = with pkgs.lib; {
              description = "Focused PDF and image viewer and editor for GNOME";
              homepage = "https://github.com/mika/review";
              license = licenses.gpl3Plus;
              mainProgram = "review";
              platforms = platforms.linux;
            };
          };
        in {
          default = review;
          review = review;
        });

      apps = nixpkgs.lib.genAttrs systems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/review";
          meta.description = "Review PDF and image viewer";
        };
      });
    };
}
