# Third-party notices

ClipItTo is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).
It uses the components below, under their own licenses, all compatible with the GPL-3.0.

| Component | License | In the Ubuntu package | In the Windows build | In the macOS app |
|---|---|---|---|---|
| SQLite | public domain | built in | built in | built in |
| SQLite3 Multiple Ciphers (with the parts listed below) | MIT | built in | built in | built in |
| toml++ | MIT | system package (not shipped) | built in | built in |
| Monocypher | CC0-1.0 or BSD-2-Clause | built in | built in | built in |
| QR Code generator library (Project Nayuki) | MIT | built in | built in | built in |
| Qt 6 | LGPL-3.0 | system packages (not shipped) | DLLs shipped, unmodified | frameworks shipped, unmodified |
| QtKeychain | BSD-3-Clause | system package (not shipped) | not used | not used (the macOS Keychain, through Apple's Security framework) |
| Tesseract OCR | Apache-2.0 | system package (Depends: libtesseract5) | not used (Windows.Media.Ocr, part of Windows) | not used (the Vision framework, part of macOS) |
| Leptonica | BSD-2-Clause | system package (a dependency of Tesseract) | not used | not used |
| tessdata_fast `eng.traineddata` (English text-recognition data) | Apache-2.0 | system package (Recommends: tesseract-ocr-eng) | not used | not used |
| CTranslate2 | MIT | built in (static) | built in (static) | built in (static) |
| SentencePiece | Apache-2.0 | built in (static) | built in (static) | built in (static) |
| cpu_features, ruy (bundled with CTranslate2) | Apache-2.0 | built in (static) | built in (static) | built in (static) |
| cpuinfo, clog (bundled with ruy) | BSD-2-Clause | built in (static) | built in (static) | built in (static) |
| spdlog (bundled with CTranslate2) | MIT | built in (static) | built in (static) | built in (static) |
| Translation models (OPUS-MT, as packaged by Argos Translate) | CC BY 4.0 | not shipped: added by the user | not shipped | not shipped |
| Microsoft Visual C++ runtime | Microsoft redistributable | not used | DLLs shipped | not used |

The AppImage and the snap carry the Ubuntu 24.04 libraries they use (Qt 6 under the LGPL-3.0,
toml++, QtKeychain, Tesseract and Leptonica, and their dependencies), unmodified, and the English
text-recognition data (`eng.traineddata`, from tessdata_fast 4.1.0, Apache-2.0).

## SQLite

The SQLite source code is in the public domain: https://www.sqlite.org/copyright.html

## SQLite3 Multiple Ciphers

https://github.com/utelle/SQLite3MultipleCiphers (used for the encrypted history)

Copyright (c) 2006-2026 Ulrich Telle

It includes:
- AEGIS code, Copyright (c) 2023-2024 Frank Denis (MIT)
- miniz, Copyright 2013-2014 RAD Game Tools and Valve Software, Copyright 2010-2014 Rich
  Geldreich and Tenacious Software LLC, Copyright 2016 Martin Raiber (MIT)
- a SHA-2 implementation, Copyright (C) 2005, 2007 Olivier Gay (BSD-3-Clause, below)
- public-domain code by Alexander Peslyak (MD5), Thomas Pornin and Keith Medcalf

The MIT license of the parts above:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this
> software and associated documentation files (the "Software"), to deal in the Software
> without restriction, including without limitation the rights to use, copy, modify, merge,
> publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
> to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
> INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
> PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
> FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
> DEALINGS IN THE SOFTWARE.

The SHA-2 implementation:

> Copyright (C) 2005, 2007 Olivier Gay <olivier.gay@a3.epfl.ch>
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without modification, are
> permitted provided that the following conditions are met:
> 1. Redistributions of source code must retain the above copyright notice, this list of
>    conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice, this list of
>    conditions and the following disclaimer in the documentation and/or other materials
>    provided with the distribution.
> 3. Neither the name of the project nor the names of its contributors may be used to endorse
>    or promote products derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR
> IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
> AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE PROJECT OR
> CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
> THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
> OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

## toml++

https://github.com/marzer/tomlplusplus (reads config.toml)

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Licensed under the MIT license quoted above.

## Monocypher

https://monocypher.org (version 4.0.2; the crypto behind password copies: Argon2id,
X25519, XChaCha20-Poly1305, BLAKE2b). Dual-licensed CC0-1.0 or BSD-2-Clause; ClipItTo uses
it under the BSD-2-Clause license:

> Copyright (c) 2017-2023, Loup Vaillant
> Copyright (c) 2017-2019, Michael Savage
> Copyright (c) 2017-2023, Fabio Scotoni
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without modification, are
> permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this list of
>    conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright notice, this list of
>    conditions and the following disclaimer in the documentation and/or other materials
>    provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
> OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
> COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
> EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
> HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
> TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
> SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## QR Code generator library

https://www.nayuki.io/page/qr-code-generator-library (version 1.8.0, `cpp/qrcodegen.cpp` and `.hpp`; the QR view of
a copy). MIT License:

> Copyright (c) Project Nayuki.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of
> this software and associated documentation files (the "Software"), to deal in
> the Software without restriction, including without limitation the rights to
> use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
> the Software, and to permit persons to whom the Software is furnished to do so,
> subject to the following conditions:
> - The above copyright notice and this permission notice shall be included in
>   all copies or substantial portions of the Software.
> - The Software is provided "as is", without warranty of any kind, express or
>   implied, including but not limited to the warranties of merchantability,
>   fitness for a particular purpose and noninfringement. In no event shall the
>   authors or copyright holders be liable for any claim, damages or other
>   liability, whether in an action of contract, tort or otherwise, arising from,
>   out of or in connection with the Software or the use or other dealings in the
>   Software.

## Qt 6

https://www.qt.io (Qt Core, Gui, Widgets and Network; D-Bus on Linux; the Windows build also
ships Qt Svg and Qt's platform and image plugins)

Copyright (C) The Qt Company Ltd. and other contributors. Licensed under the GNU Lesser General
Public License v3.0; the text is in [licenses/LGPL-3.0.txt](licenses/LGPL-3.0.txt), and the
LGPL-3.0 builds on the GPL-3.0 in [LICENSE](LICENSE).

The Windows build ships the unmodified Qt 6.5.3 DLLs next to ClipItTo, which loads them
dynamically: you may replace them with your own build of Qt 6.5. The Qt source code is at
https://download.qt.io/archive/qt/6.5/6.5.3/. Qt itself includes third-party code; see
https://doc.qt.io/qt-6/licenses-used-in-qt.html.

## QtKeychain

https://github.com/frankosterfeld/qtkeychain (on Linux, keeps the history key in the system
keyring). BSD-3-Clause. Used from the distribution's package, not shipped with ClipItTo.

## Microsoft Visual C++ runtime

The Windows build includes Microsoft's redistributable Visual C++ runtime DLLs
(e.g. msvcp140.dll), distributed under Microsoft's redistribution terms for the Visual Studio
runtime libraries.


## Offline translation

CTranslate2 (https://github.com/OpenNMT/CTranslate2, MIT, Copyright SYSTRAN) runs the translation models and
SentencePiece (https://github.com/google/sentencepiece, Apache-2.0, Copyright Google) cuts the text into pieces. They
are built from source at pinned versions by `scripts/build-translation-libs.*` and linked into ClipItTo; their licence
texts are installed under `share/doc/clipitto/translation-licenses`. The translation models are not part of ClipItTo:
users add them. The Argos Translate packages are derived from OPUS-MT models by Jörg Tiedemann and Santhosh
Thottingal ("OPUS-MT — Building open translation services for the World", EAMT 2020), licensed CC BY 4.0
(https://creativecommons.org/licenses/by/4.0/).

## If ClipItTo's own licence ever changes

A checklist, not legal advice. ClipItTo is GPL-3.0 today; every component above is compatible with that. If a
version is ever released under another licence, each component has to be looked at again:

| Component | Can stay in a build under another licence? | What to do |
|---|---|---|
| SQLite, SQLite3 Multiple Ciphers, toml++, Monocypher, QR Code generator, CTranslate2, spdlog | Yes (public domain / MIT / CC0 / BSD) | Keep the copyright notices and licence texts. |
| SentencePiece, Tesseract, cpu_features, ruy | Yes (Apache-2.0) | Keep the licence and NOTICE texts; say so when a file is changed. |
| Leptonica, cpuinfo, QtKeychain | Yes (BSD) | Keep the copyright notices. |
| Qt 6 | Only as LGPL-3.0: linked dynamically, users able to replace it | Ship the LGPL text and keep it as shared libraries. Other Qt licences need a commercial Qt licence. |
| Translation models | They are not part of ClipItTo | Keep the CC BY 4.0 credit where they are described. |
| Windows Hello, Vision, Windows.Media.Ocr | System components | Nothing to ship. |

Nothing above is GPL-only, so nothing has to be removed for a relicence, except that Qt needs its LGPL conditions or a
commercial licence. ClipItTo's own code would need the agreement of everyone who contributed to it.
