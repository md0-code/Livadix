# Third-party components

| Component | Use | License |
|---|---|---|
| [SDL 2.32.10](https://www.libsdl.org/) | Window, OpenGL, input and sound output; fetched at build time and linked statically | zlib |
| [libyaml 0.2.5](https://github.com/yaml/libyaml/tree/0.2.5) | YAML syntax of `livadix.yaml` (`src/third_party/libyaml`); linked statically | MIT, below |

Build and test tools, not part of the program: [GNU binutils](https://www.gnu.org/software/binutils/)
2.40 (GPL 3.0; its output is not covered by it) assembles Livadix's 68k and
PowerPC guest code (`tools/guest_build.py`; the images are committed in
`src/hw/guest/bin/`), and [mpmath](https://mpmath.org/) (BSD), when
installed, is the high-precision reference of `tests/fpu_suite.py`.

No Macintosh ROM, system software or disk image is included.

## libyaml

```
Copyright (c) 2017-2020 Ingy döt Net
Copyright (c) 2006-2016 Kirill Simonov

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
