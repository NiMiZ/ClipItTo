# Translation

Back to the [README](../README.md).

ClipItTo translates text and the text in pictures **on your computer**. Nothing you copy is sent anywhere, and
ClipItTo never downloads anything by itself.

## How it works

- Translation is done by small neural models, one per direction ("language packs": Chinese → English, English →
  Chinese, Hebrew → English, ...). They are run by [CTranslate2](https://github.com/OpenNMT/CTranslate2) (MIT) with
  [SentencePiece](https://github.com/google/sentencepiece) (Apache-2.0), both built into ClipItTo as libraries that
  have no network code.
- The packs are the Argos Translate models, which are the OPUS-MT models by Jörg Tiedemann and Santhosh
  Thottingal, licensed **CC BY 4.0**. They are not part of ClipItTo; you add the ones you need.
- A pack is a `.argosmodel` file of about 70-150 MB. The list of packs and their links is the
  [argospm index](https://github.com/argosopentech/argospm-index); for example
  `https://argos-net.com/v1/translate-zh_en-1_9.argosmodel` (Chinese → English). Download one in your browser,
  then **Settings › Translation › Add language pack…**. ClipItTo unpacks only what it needs into its data folder
  (`~/.local/share/clipitto/translation`, `%APPDATA%`/`%LOCALAPPDATA%\clipitto\translation`,
  `~/Library/Application Support/clipitto/translation`). *Remove* deletes it.

## Using it

- **Text:** select a copy, press Shift+Enter, choose **Translate** and a pack. The packs offered are those that
  translate *from* the language the text is written in (Chinese characters → the Chinese packs, Hebrew letters →
  the Hebrew pack, Latin letters → the English and other Latin-script packs). The translation is pasted; the stored
  copy stays as it was.
- **Pictures:** Shift+Enter on a picture, **Translate text from picture** and a pack: the text is read first
  (see *Text from a picture* in [Using it](usage.md)) and then translated. Reading Chinese needs Chinese text
  recognition: on Windows the Chinese language with its text-recognition feature, on Linux
  `tesseract-ocr-chi-sim`, macOS reads it by itself.
- A long text is translated a sentence at a time, and line breaks are kept.

## Good to know

- The first translation after starting ClipItTo takes a second or two while the model loads; the next ones are
  quick. Quality is that of small open models: good for understanding, not for publishing.
- A pack translates one direction. Add both if you need both (Chinese → English and English → Chinese).
- Where the build has no translation engine (a build made without the libraries), the Translation tab and the menu
  entries are absent.
- Translating is private by construction, and you can check it: the libraries import no network functions, and a
  translation runs inside a Linux sandbox with no network at all (`unshare -rn`).
- **If a translation seems stuck:** the popup stops waiting after 30 seconds and says so, and the next translation can
  start. ClipItTo writes a small log, `translation/translation.log` in its data folder (`%APPDATA%` or
  `%LOCALAPPDATA%\clipitto` on Windows, `~/.local/share/clipitto` on Linux, `~/Library/Application Support/clipitto` on
  macOS), with the time, the pack, the length of the text, and how long each translation took. It never holds the text
  itself. The engine uses one thread (`CLIPITTO_TRANSLATE_THREADS=N` in the environment changes it).

