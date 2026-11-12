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


## Checking the quality of your packs

Whether a word is translated well cannot be judged by an automatic test, so there is a small tool for looking at it
yourself. It runs a list of words and sentences through the packs you have installed and prints, for each line,
`input -> output (milliseconds, pack)`:

```
scripts/translate-check.sh                       # the starter list, scripts/translate-wordlist.txt
scripts/translate-check.sh -p he-en mylist.txt   # your own list, one pack only
scripts/translate-check.sh -o report.txt         # the report into a file
```

- Each line is translated with the first installed pack that translates from its language (the same choice the popup
  makes); a line with no fitting pack says `NO PACK`.
- `UNCHANGED` marks an answer equal to the input (often a word the model does not know), `SLOW` a line that took more
  than 3 seconds, `ERROR` a failure or a timeout (10 seconds per line). The last line counts them.
- The starter list has the Hebrew word מכשול and a few dozen Hebrew, Russian, Chinese and English words and sentences,
  numbers, links and code. Add the words that went wrong for you to your own list: `#` lines are comments.
- The tool is the hidden option `clipitto-daemon --translate-check <list> [--translate-pack <id>] [--translate-out <file>]`;
  the script only finds the program (`-d` or `CLIPITTO_DAEMON` to name it). On Windows run the program directly with
  `--translate-out report.txt` (it has no console). It needs the real engine and real packs, so CI cannot run it, and
  it never touches the clipboard or the history. The list is read, not logged.
