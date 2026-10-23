# Application icon

`appicon.png` is the shared Sailor application icon. It is a lossless PNG conversion of the 640 x 640 JPEG attached to [issue #193](https://github.com/aantropov/sailor/issues/193), with no crop or artwork changes. The original JPEG SHA-256 is `9adfb6cb70333f0fa59e562433714e40e60463845eaead29d67f9345692ed4db`.

The editor's MAUI build generates its platform icons from this PNG. The macOS runtime embeds the PNG at configure time so standalone launches do not depend on the working directory. Windows embeds `appicon.ico` in both the engine executable and runtime library; engine windows load their icons from the library even when hosted by another executable. The MCP bridge uses the same ICO for its managed executable and Windows app host.

To regenerate the Windows icon on macOS after updating the PNG:

```sh
python3 Resources/AppIcon/generate_windows_icon.py
```

The ICO contains 16, 32, 48, 64, 128 and 256 pixel images. It is checked in, so Windows builds do not require image conversion tools.
