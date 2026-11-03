# LEGO Mods 0.5.16

> **WARNING: AI-GENERATED**
>
> LEGO Mods is a project created by Codex AI. My sole aim was to explore its capabilities: could a tool like this build a simple `.asi` file redirector for modding and translation? During testing, I also had various safeguards added. I welcome feedback from experienced developers, so this project does not become just another piece of AI slop in the vast ocean of the internet.

## What it does

LEGO Mods lets supported LEGO games load modified files from the `mods` folder, keeping the original folder structure. It supports text, fonts, textures and other resources in formats the game understands. When no usable replacement is available, the game uses its original files, including files in archives.

The module supports both 32-bit and 64-bit games. Use the build that matches the **game executable**, regardless of your Windows architecture.

## Installation

1. Install an ASI loader separately. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) is supported; version 10.0.2 was used for testing. Choose its **x86** or **x64** `dinput8.dll` to match the game and place it beside the game executable. The loader is **not included** in LEGO Mods.
2. Download the matching LEGO Mods **x86** or **x64** release and copy `LegoMods.asi` directly beside the game executable. `LegoMods.ini` is optional.
3. Put replacement files in `mods`, preserving their original paths. For example, `STUFF\TEXT\TEXT.CSV` becomes `mods\STUFF\TEXT\TEXT.CSV`.
4. Keep the original archives in place and launch the game as usual. For Steam games, use Steam if direct EXE launch fails.

Keep only one active copy of `LegoMods.asi`. If an old copy is in `scripts`, remove that copy when installing beside the executable. Preserve your existing translations and INI settings when updating.

`global.ini` is not required by LEGO Mods. If you already have a loader configuration, keep `DontLoadFromDllMain=1` and allow ASI loading from the game directory. LEGO Mods performs its own resource redirection; do not point the loader's separate `OverloadFromFolder` feature at the same replacement folder.

## INI files

### LegoMods.ini

Place this optional UTF-8 file beside the game executable:

```ini
[Files]
ModsFolder=mods

[Logging]
Enabled=0
```

- **ModsFolder:** one folder directly beside the executable. For example, `ModsFolder=translations` uses `translations` instead of `mods` for both resources and `ENGLISH_PLUS` files. Keep the original subfolder structure inside it.
- **Enabled:** `0` disables logging; `1` enables `LegoMods.log` beside the ASI. The external ASI loader has its own logging settings.

Without this INI, the module uses `mods` and logging stays off. Settings are read once at startup; restart the game after editing them.

### ENGLISH_PLUS files

These optional files replace menu and input labels stored inside the executable. Place the matching INI in `mods`, or in the folder selected by `ModsFolder`.

| Game | INI file |
|---|---|
| LEGO Batman: The Videogame | `ENGLISH_PLUS_LB1.ini` |
| LEGO Indiana Jones: The Original Adventures | `ENGLISH_PLUS_IJ1.ini` |
| LEGO Indiana Jones 2: The Adventure Continues | `ENGLISH_PLUS_IJ2.ini` |
| LEGO Star Wars: The Complete Saga - GOG | `ENGLISH_PLUS_TCS.ini` |

The supplied templates contain Hungarian translations. Keep the game language set to English. Save INIs as UTF-8, edit only the text after `=`, respect the byte limits noted in each file, and preserve any required `%d` and `%s` placeholders. Missing, empty or invalid values keep the original label. Restart the game after editing an INI. Custom text still needs matching glyphs in the game's font.

## Supported games

Support is matched by the executable's full SHA-256 hash, rather than its installed filename. The list covers English-language releases. **Original filename** is the name recorded in the executable and can differ from the installed name.

**Tested** means that the edition has been confirmed working in gameplay testing. It does not mean that every edition was retested with every module update.

| Game / edition | Original filename | SHA-256 | Status |
|---|---|---|---|
| LEGO Batman: The Videogame (Epic, 32-bit) | `LEGOBatman.exe` | `63959c58667e7b1678523dd9a7404ff99acbcd634beaf049ee2cce139ea5e916` | Tested |
| LEGO Batman: The Videogame (GOG, 32-bit) | `LEGOBatman.exe` | `981383d8c842a919dfecf61caf808a27745c3af39f4fa6197e3da88f5e6c842f` | Tested |
| LEGO Batman: The Videogame (Steam, 32-bit) | `LEGOBatman.exe` | `f9ef801cc39df83cac5f9c8f35b6724cd0587f8df6754a9601be7c35aa960716` | Tested |
| LEGO Batman 2: DC Super Heroes (Epic, 32-bit) | `LEGOBatman2.exe` | `5f2775f4802a07487f901a392ff78f1a7884ca3ed0c61c67e38944c9a4b45b3b` | Tested |
| LEGO Batman 2: DC Super Heroes (GOG, 32-bit) | `LEGOBatman2.exe` | `849984767c19c3d56eae31e3f4ff54418af1ddbf5d01e03b9eefa8ecc1ad20dc` | Tested |
| LEGO Batman 2: DC Super Heroes (Steam, 32-bit) | `LEGOBatman2.exe` | `d4c66368751ff99d21674cbc1d2f57cba4b68241f848932ea77543bf701b43e4` | Tested |
| LEGO Batman 3: Beyond Gotham (Epic, 32-bit, DX9) | `LEGOBatman3.exe` | `6cd6631a42f8b4e69b902531c46d1fbf23e9eb33933f407818060ba317657d92` | Tested |
| LEGO Batman 3: Beyond Gotham (Epic, 64-bit, DX11) | `LEGOBatman3.exe` | `1be1339725d9aa754a90f8036b34f9562c92e64a9b12d2f71eb382e90193f5f2` | Tested |
| LEGO Batman 3: Beyond Gotham (GOG, 32-bit, DX9) | `LEGOBatman3.exe` | `154c361b084c2df792194ac306b5b765dc868870404a019ad1c175979eb698e2` | Tested |
| LEGO Batman 3: Beyond Gotham (GOG, 64-bit, DX11) | `LEGOBatman3.exe` | `2f599424414b7c254e6ce2a197525a9ad71a3c31ab2c67e83f1a255dac16be9a` | Tested |
| LEGO Batman 3: Beyond Gotham (Steam, 32-bit, DX9) | `LEGOBatman3.exe` | `5e2232f5070e6718ad0de714f4bc1d138b2ab52042334af074659e687c4ee6c8` | Tested |
| LEGO Batman 3: Beyond Gotham (Steam, 64-bit, DX11) | `LEGOBatman3.exe` | `4977c11924f1f0c1e7149d62bf2aa7e83bb7862c6424ed9070ccf5b3371d0a0d` | Tested |
| LEGO City Undercover (GOG, 64-bit, DX11) | `LEGOLCUR.exe` | `624fed07274bbd105d1d571ecb97a290df6f6f0aff76898c516890da6d136f76` | Tested |
| LEGO City Undercover (Steam, 64-bit, DX11) | `LEGOLCUR.exe` | `624fed07274bbd105d1d571ecb97a290df6f6f0aff76898c516890da6d136f76` | Tested |
| LEGO DC Super-Villains (Steam, 64-bit, DX11) | `LEGO DC Super-villains.exe` | `072c4725ea7ca4c88b1cb74b64b3536327c59a158ee91a13224dda556727b0c1` | Tested |
| LEGO Harry Potter: Years 1–4 (GOG, 32-bit) | `LEGOHarryPotter.exe` | `deec85b342fb59fdfe93cf491687a15ab3cca766379fdf1458d806d23997f19f` | Tested |
| LEGO Harry Potter: Years 1–4 (Steam, 32-bit) | `LEGOHarryPotter.exe` | `6ee3dc4c3062972545f9ff4c3f6707e5d493a42e8e15faf1452abbe509854b00` | Tested |
| LEGO Harry Potter: Years 5–7 (GOG, 32-bit) | `LEGOHarry2.exe` | `0159f149aee484bd2076a312716dcd19e10993062875fd172714b0ef9598f1ff` | Tested |
| LEGO Harry Potter: Years 5–7 (Steam, 32-bit) | `LEGOHarry2.exe` | `d933f68e1f7cf738933390aa070166123c27958656ac72d6bd03706ff47a74f0` | Tested |
| LEGO Indiana Jones 2: The Adventure Continues (GOG, 32-bit) | `LEGOIndy2.exe` | `1f0f51faf303c446fc21052a10d1cfa1597022f3712f810837119da2af00eb7a` | Tested |
| LEGO Indiana Jones 2: The Adventure Continues (Steam, 32-bit) | `LEGOIndy2.exe` | `b3d3a7871187dbb6bd5d357e93a8153585ac68fbe3a302bae7e4ccb372d74ede` | Tested |
| LEGO Indiana Jones: The Original Adventures (GOG, 32-bit) | `LEGOIndy.exe` | `6c72717abc5b38a44ec235540176c76a2778c1d8f0dd8889b714a12391e162d1` | Tested |
| LEGO Indiana Jones: The Original Adventures (Steam, 32-bit) | `LEGOIndy.exe` | `c12f7348ec63aed61f56eb15d825f15b8340500668fa8c8dcf06c2f332e5e5fd` | Tested |
| LEGO Jurassic World (Steam, 32-bit, DX9) | `LEGOJurassicWorld.exe` | `946edb4f67f47931702fe92410cc6dc33004c7bbd7b60caa929ce224d31c7367` | Tested |
| LEGO Jurassic World (Steam, 64-bit, DX11) | `LEGOJurassicWorld.exe` | `5b6c09682d830307e6d4eedc1c72ac06f3b8d6ff55eb185c63bd68a3fda4f3ba` | Tested |
| LEGO Marvel Super Heroes (Steam, 32-bit) | `LEGOMarvel.exe` | `08b7018182edf6cd0ab13ec337368febfaf0713e3b58bf04d2c6d87626c25856` | Tested |
| LEGO Marvel Super Heroes 2 (Steam, 32-bit, DX9) | `LEGOMARVEL2.exe` | `af8e59fbba7aefd73553677b6df4731bd7f39e809ffa44a24b66021cf07094d8` | Tested |
| LEGO Marvel Super Heroes 2 (Steam, 64-bit, DX11) | `LEGOMARVEL2.exe` | `acba1c3c0c41eb0d604ce5864838c6b81da756faed7027a784c4fae88b325af8` | Tested |
| LEGO Marvel's Avengers (Steam, 32-bit, DX9) | `LEGOMARVELAvengers.exe` | `bc7e7ad08c92dfb8d285a7e5c67af8a0bfe767eb6651a3d5d8ad9df20c927367` | Tested |
| LEGO Marvel's Avengers (Steam, 64-bit, DX11) | `LEGOMARVELAvengers.exe` | `e5d37bf0b5fce6f46a529ad7eb478ccbb9d98d517ad7f14610c6253a7d8375a9` | Tested |
| LEGO Pirates of the Caribbean: The Video Game (GOG, 32-bit) | `LEGOPirates.exe` | `39de791e932a45af6dc5c002d701cd6cdac821b92f7b84166599065348dedc69` | Tested |
| LEGO Star Wars III: The Clone Wars (GOG, 32-bit) | `LEGOCloneWars.exe` | `0f62acd337d0b8cf39789f03335020fea4441d4f523e11018d4024ade2797ba0` | Tested |
| LEGO Star Wars: The Complete Saga (GOG, 32-bit) | `LEGOStarWarsSaga.exe` | `239f6c09b73e5764ced7469f158b621ff34c9984b5b0a850fe77248add259113` | Tested |
| LEGO Star Wars: The Force Awakens (Steam, 32-bit, DX9) | `LEGOSWTFA.exe` | `b3afb9078486fa72aec54d695620d9a1f1568b3cfe23f4db200d86a793ef7037` | Tested |
| LEGO Star Wars: The Force Awakens (Steam, 64-bit, DX11) | `LEGOSWTFA.exe` | `754350ea0063a08fd0217568129a10950204ebe24d017284119c8afb87fe0ec9` | Tested |
| LEGO Star Wars: The Skywalker Saga (Epic, 64-bit, DX11) | `LEGOSTARWARSSKYWALKERSAGA.exe` | `10f0e68dc406af43a73ea0b59571d7d86f5c741b63cc3f1a6d2e45b0d581b22b` | Tested |
| LEGO The Hobbit (GOG, 32-bit, DX9) | `LEGOHobbit.exe` | `072e25e2c0be8619d92acd4ae14319164d2ee1cdb1425a295ec2e0f6e2f5c998` | Tested |
| LEGO The Hobbit (GOG, 64-bit, DX11) | `LEGOHobbit.exe` | `a7a8956e73007e7f04302eef63c97ba8adf832f7e82ff53770c2a4fd4dd084a0` | Does not work |
| LEGO The Hobbit (Steam, 32-bit, DX9) | `LEGOHobbit.exe` | `072e25e2c0be8619d92acd4ae14319164d2ee1cdb1425a295ec2e0f6e2f5c998` | Tested |
| LEGO The Hobbit (Steam, 64-bit, DX11) | `LEGOHobbit.exe` | `a7a8956e73007e7f04302eef63c97ba8adf832f7e82ff53770c2a4fd4dd084a0` | Does not work |
| LEGO The Lord of the Rings (GOG, 32-bit) | `LEGOLOTR.exe` | `02ab7725562eccd1a2399f50aae5c64371bd21383058cd484fd6c81ce4ab7f0a` | Tested |
| LEGO The Lord of the Rings (Steam, 32-bit) | `LEGOLOTR.exe` | `b8adddb3682029fe234a0d9fd91780377f475757cbc7025868c6be393a4c7a1d` | Tested |
| The LEGO Movie 2 Videogame (GOG, 64-bit, DX11) | `LEGO The LEGO Movie 2.exe` | `f16f5fbeff7d27be498b3ee5c3c891412e05cf9e7fcb45f4842db1f2e8a75339` | Tested |
| The LEGO Movie Videogame (GOG, 32-bit, DX9) | `LEGOEmmet.exe` | `a64e6d974915f511687b246dc92394603993d9f8c074710d3d9038947c85f01d` | Tested |
| The LEGO Movie Videogame (Steam, 32-bit, DX9) | `LEGOEmmet.exe` | `a64e6d974915f511687b246dc92394603993d9f8c074710d3d9038947c85f01d` | Tested |
| The LEGO NINJAGO Movie Video Game (GOG, 64-bit, DX11) | `LEGONINJAGO.exe` | `671f6a6e66acc2ef28094277a2d91ae881df58297c29415c8a9e4b9db6367233` | Tested |
| The LEGO NINJAGO Movie Video Game (Steam, 32-bit, DX9) | `LEGONINJAGO.exe` | `b00dd24b617063fbe9a8de9ae98ee47bb144d0be3d5595927ba4cd746b4b0f09` | Tested |
| The LEGO NINJAGO Movie Video Game (Steam, 64-bit, DX11) | `LEGONINJAGO.exe` | `671f6a6e66acc2ef28094277a2d91ae881df58297c29415c8a9e4b9db6367233` | Tested |

**LEGO The Hobbit's 64-bit/DX11 version does not work.** The developer left this version unfixed. Use the 32-bit/DX9 version.

## Building from source

Use Windows with Visual Studio 2022 or compatible Build Tools, the Desktop development with C++ workload, a Windows SDK, and Python 3.10 or later. MinHook is vendored in the repository; no ASI loader is needed to compile the module.

Run these commands from the repository root:

```powershell
python source/tools/generate_profiles.py --check
python source/tools/generate_readme.py --check
.\source\build.ps1 -Architecture All -CompileTests
python source/package.py --output-dir dist
```

The build creates `source/build/x86/LegoMods.asi` and `source/build/x64/LegoMods.asi`. `-CompileTests` compiles the native fixtures without running them. The packaging command creates separate source, x86 and x64 ZIPs with SHA-256 manifests. Game executables, resources, local logs and external loader files are excluded.

Edit `source/profiles.json` and regenerate `source/src/generated_profiles.inc` with `python source/tools/generate_profiles.py` when adding a reviewed native profile. The support table is generated from `SUPPORTED_BUILDS.json`; update it with `python source/tools/generate_readme.py`.

## License

The project code is licensed under [MIT](LICENSE). MinHook and its bundled disassembler retain their own notices; see [Third-party notices](THIRD_PARTY_NOTICES.md).
