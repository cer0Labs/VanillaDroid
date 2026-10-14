# Third-party notices · Avisos de terceros

VanillaDroid's own code is free software under the GNU General Public License version 3 ([LICENSE](LICENSE)).
VanillaDroid (the Android app) and VanillaDroid Setup (the program for Windows and macOS, with the console tool
`vdsetup` and the data baker `vd_baker`) are built with the software and art listed here, each used under the license
given. The full license texts follow the lists.

El código propio de VanillaDroid es software libre con la licencia GNU General Public License versión 3
([LICENSE](LICENSE)). VanillaDroid (la app de Android) y VanillaDroid Setup (el programa para Windows y macOS, con la
herramienta de consola `vdsetup` y el horneador de datos `vd_baker`) usan el software y el arte de estas listas, cada
uno con la licencia que se indica. Los textos completos de las licencias van al final.

No file of World of Warcraft is distributed: the game data is made on the player's own computer from the player's own
copy of the game. · No se reparte ningún fichero de World of Warcraft: los datos del juego se preparan en el ordenador
de cada jugador a partir de su propia copia del juego.

## Software

| Software | Version | License | In |
|---|---|---|---|
| Dear ImGui (Omar Cornut) | 1.92.9b | MIT | Setup |
| stb_truetype, stb_rect_pack, stb_textedit (Sean Barrett), inside Dear ImGui | — | MIT or public domain | Setup |
| SDL, Simple DirectMedia Layer | 3.4.18 | zlib | Setup |
| QR Code generator (Project Nayuki) | 1.8.0 | MIT | Setup |
| StormLib (Ladislav Zezula) | 9.40 | MIT | Setup |
| bzip2 / libbzip2 (Julian Seward), inside StormLib | 1.0.5 | bzip2 license (BSD-style) | Setup |
| LibTomCrypt, LibTomMath, LZMA SDK, lookup3, inside StormLib | — | public domain | Setup |
| astc-encoder (Arm Limited) | 5.7.0 | Apache 2.0 | Setup |
| LZ4 (Yann Collet) | 1.10.0 | BSD 2-Clause | App, Setup |
| zlib (Jean-loup Gailly, Mark Adler); also the copy inside StormLib | 1.3.2 | zlib | Setup |
| Mbed TLS (The Mbed TLS Contributors) | 3.6.7 | Apache 2.0 | App, Setup |
| miniaudio (David Reid) | 0.11.25 | public domain or MIT-0 | App, Setup |
| GLM, OpenGL Mathematics (G-Truc Creation) | 1.0.3 | MIT | App, Setup |
| volk (Arseny Kapoulkine) | vulkan-sdk-1.4.363.0 | MIT | App |
| Vulkan Memory Allocator (AMD) | 3.4.0 | MIT | App |
| Vulkan-Headers (The Khronos Group) | vulkan-sdk-1.4.363.0 | Apache 2.0 or MIT | App |
| Android Jetpack: games-activity, games-frame-pacing, games-controller, appcompat, core and the AndroidX libraries they use (The Android Open Source Project) | 4.4.2, 2.1.3, 2.0.2, 1.7.1, 1.13.1 | Apache 2.0 | App |
| Kotlin standard library (JetBrains) | 1.8.22 | Apache 2.0 | App |
| LLVM libc++ (`libc++_shared.so` of the Android NDK) | NDK 28.2 | Apache 2.0 with LLVM exception | App |

Mbed TLS is offered under Apache-2.0 OR GPL-2.0-or-later: VanillaDroid uses it under Apache 2.0. miniaudio is offered
as public domain (Unlicense) or MIT No Attribution: VanillaDroid takes it as public domain. GLM is offered under the
Happy Bunny License or MIT: VanillaDroid uses it under MIT. Vulkan-Headers are offered under Apache-2.0 OR MIT:
VanillaDroid uses them under MIT. LLVM's runtime libraries are under Apache 2.0 with the LLVM exception, which needs no
notice in binary form; it is listed here anyway.

## Data from other projects · Datos de otros proyectos

| Project | What | License |
|---|---|---|
| [vMaNGOS](https://github.com/vmangos/core) (MaNGOS, MaNGOSZero, Nostalrius and Elysium contributors) | The protocol tables (`engine/src/net/*.gen.*`: opcodes, update fields, response codes), generated from its sources by `scripts/gen-protocol.ps1`; the patrol coordinates of the walking benchmarks (`engine/src/app/WalkTest.cpp`), from its world database | GPL 2.0 or later, used here under the GPL 3.0 |

## Art and fonts · Arte y fuentes

| Art | Use | Origin | License |
|---|---|---|---|
| Droid Sans 1.00 (Google; Ascender Corporation) | The app's own font and the Setup's window | "Digitized data copyright © 2007, Google Corporation". "Droid" is a trademark of Google | Apache 2.0 |
| `jump` icon | The touch button Jump | "Jump To Top", [SVG Repo 374927](https://www.svgrepo.com/svg/374927/jump-to-top) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), modified (see below) |
| `mount` icon | The touch button Mount | "Horse Head Outline", [SVG Repo 173978](https://www.svgrepo.com/svg/173978/horse-head-outline) | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| `target` icon | The touch button Change target | "Target", [SVG Repo 503107](https://www.svgrepo.com/svg/503107/target) | [MPL 2.0](https://mozilla.org/MPL/2.0/); its source below |
| `settings` icon | The touch button Settings | "Settings", [SVG Repo 511122](https://www.svgrepo.com/svg/511122/settings) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), modified (see below) |
| `talk` icon | The touch button Talk | "Talk Chat Message Communication Speech Conversation Interaction", [SVG Repo 338040](https://www.svgrepo.com/svg/338040/talk-chat-message-communication-speech-conversation-interaction) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), modified (see below) |
| VanillaDroid's icon | The app and the Setup | VanillaDroid (own work) | All rights reserved: not covered by the GPL |

The icons under CC BY 4.0 (Creative Commons Attribution 4.0 International, "CC Attribution License" on SVG Repo) are
credited to their authors as given on each linked SVG Repo page, which holds the author, the source and the license.
They were modified: rasterized to single-colour 128 × 128 images and tinted by the app. The CC0 icon needs no credit;
it is listed for completeness. · Los iconos con CC BY 4.0 se atribuyen a sus autores tal como figuran en la página de SVG
Repo enlazada (autor, origen y licencia). Se han modificado: rasterizados a imágenes de un color de 128 × 128 que la app
tiñe. El icono CC0 no necesita atribución; se lista por completitud.

The `target` icon is under the Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/): the app ships it rasterized,
and this is its Source Code Form, the SVG file as published on its SVG Repo page and used to make the image. · El icono
`target` tiene la Mozilla Public License 2.0: la app lo lleva rasterizado y este es su código fuente, el SVG tal como
está publicado en su página de SVG Repo y del que sale la imagen.

```xml
<?xml version="1.0" ?><!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg fill="#000000" width="800px" height="800px" viewBox="0 0 512 512" data-name="Layer 1" id="Layer_1" xmlns="http://www.w3.org/2000/svg"><title/><path d="M321.85,250.69c-4-33.61-30.39-61-65.85-61-36,0-66.34,30.31-66.34,66.34S220,322.34,256,322.34c35.47,0,61.84-27.41,65.85-61a18.39,18.39,0,0,0,.49-5.32A18.71,18.71,0,0,0,321.85,250.69ZM225.12,256c0-39.95,59.88-39.6,61.76,0C285,295.55,225.12,296,225.12,256Z"/><path d="M433.3,238.27H395c-7.27-51.81-41.57-96.15-91.93-114.52a133.34,133.34,0,0,0-29.29-7v-38c0-22.82-35.46-22.86-35.46,0V117c-34.91,4.65-68,22.22-90.69,50.08a141.57,141.57,0,0,0-30.44,71.24H78.7c-22.82,0-22.86,35.46,0,35.46H117a137.24,137.24,0,0,0,18.61,54.45c22.57,37.45,60.88,61.14,102.69,66.63V433.3c0,22.82,35.46,22.86,35.46,0V395.07c2.92-.35,5.85-.75,8.76-1.28,60.25-10.79,104-61.36,112.44-120.06H433.3C456.12,273.73,456.16,238.27,433.3,238.27ZM291.38,354.83a106,106,0,0,1-115.8-31.39c-62-73.5,5.19-188.93,99.84-170.61,49.75,9.63,84.51,53,85.5,103.17C360.05,299.87,333.32,340,291.38,354.83Z"/></svg>
```

## License texts · Textos de las licencias

### Dear ImGui - MIT License

```text
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### stb_truetype, stb_rect_pack, stb_textedit - MIT License

```text
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### SDL - zlib License

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### QR Code generator library - MIT License

```text
Copyright © 2022 Project Nayuki. (MIT License)  
[https://www.nayuki.io/page/qr-code-generator-library](https://www.nayuki.io/page/qr-code-generator-library)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

* The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

* The Software is provided "as is", without warranty of any kind, express or
  implied, including but not limited to the warranties of merchantability,
  fitness for a particular purpose and noninfringement. In no event shall the
  authors or copyright holders be liable for any claim, damages or other
  liability, whether in an action of contract, tort or otherwise, arising from,
  out of or in connection with the Software or the use or other dealings in the
  Software.
```

### StormLib - MIT License

```text
The MIT License (MIT)

Copyright (c) 1999-2013 Ladislav Zezula

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### bzip2 / libbzip2 1.0.5 (inside StormLib)

```text
This program, "bzip2", the associated library "libbzip2", and all
documentation, are copyright (C) 1996-2007 Julian R Seward.  All
rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must
   not claim that you wrote the original software.  If you use this
   software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote
   products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### LibTomCrypt, LibTomMath, LZMA SDK and lookup3 (inside StormLib) - public domain

```text
LibTomCrypt and LibTomMath (Tom St Denis): "free for all purposes without any express guarantee
it works". LZMA SDK (Igor Pavlov): public domain. lookup3.c (Bob Jenkins, 2006): public domain.
```

### LZ4 Library - BSD 2-Clause License

```text
LZ4 Library
Copyright (c) 2011-2020, Yann Collet
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib - zlib License (also the copy inside StormLib)

```text
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

### miniaudio - public domain (Unlicense) or MIT No Attribution

```text
This software is available as a choice of the following licenses. Choose
whichever you prefer.

===============================================================================
ALTERNATIVE 1 - Public Domain (www.unlicense.org)
===============================================================================
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>

===============================================================================
ALTERNATIVE 2 - MIT No Attribution
===============================================================================
Copyright 2025 David Reid

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### GLM - MIT License

```text
================================================================================
OpenGL Mathematics (GLM)
--------------------------------------------------------------------------------
GLM is licensed under The Happy Bunny License or MIT License

================================================================================
The Happy Bunny License (Modified MIT License)
--------------------------------------------------------------------------------
Copyright (c) 2005 - G-Truc Creation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Restrictions:
 By making use of the Software for military purposes, you choose to make a
 Bunny unhappy.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

================================================================================
The MIT License
--------------------------------------------------------------------------------
Copyright (c) 2005 - G-Truc Creation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### volk - MIT License

```text
Copyright (c) 2018-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Vulkan Memory Allocator - MIT License

```text
Copyright (c) 2017-2026 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Vulkan-Headers - MIT License

```text
MIT License

Copyright (c) 2015-2023 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Apache License 2.0 - astc-encoder, Mbed TLS, Droid Sans, Android Jetpack, Kotlin standard library

```text
astc-encoder: Copyright 2011-2026 Arm Limited.
Mbed TLS: Copyright The Mbed TLS Contributors.
Droid Sans: Digitized data copyright (c) 2007, Google Corporation. "Droid" is a trademark of Google.
Android Jetpack libraries: Copyright The Android Open Source Project.
Kotlin standard library: Copyright 2010-2023 JetBrains s.r.o. and Kotlin Programming Language
contributors.


                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.
```

### LLVM libc++ - Apache License 2.0 with LLVM Exceptions

```text
The LLVM Project is under the Apache License v2.0 with LLVM Exceptions (the Apache License 2.0 text is above).

---- LLVM Exceptions to the Apache 2.0 License ----

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into an Object form of such source code, you
may redistribute such embedded portions in such Object form without complying
with the conditions of Sections 4(a), 4(b) and 4(d) of the License.

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 ("Combined Software") and if a
court of competent jurisdiction determines that the patent provision (Section
3), the indemnity provision (Section 9) or other Section of the License
conflicts with the conditions of the GPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of
the License, but only in their entirety and only with respect to the Combined
Software.
```
