# Third-party notices

ClipItTo is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).
It uses the components below, under their own licenses, all compatible with the GPL-3.0.

| Component | License | In the Ubuntu package | In the Windows build |
|---|---|---|---|
| SQLite | public domain | built in | built in |
| SQLite3 Multiple Ciphers (with the parts listed below) | MIT | built in | built in |
| toml++ | MIT | system package (not shipped) | built in |
| Qt 6 | LGPL-3.0 | system packages (not shipped) | DLLs shipped, unmodified |
| QtKeychain | BSD-3-Clause | system package (not shipped) | not used |
| Microsoft Visual C++ runtime | Microsoft redistributable | not used | DLLs shipped |

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
