# Additional permissions to the GPL / Дополнительные разрешения к GPL

openRTS is licensed under the GNU General Public License, version 3
(`LICENSE`). Under its section 7 the copyright holders of openRTS grant the
following additional permission.

## Microsoft DirectX Shader Compiler

You may convey openRTS, and works based on it, together with the
unmodified distributable binaries of the Microsoft DirectX Shader Compiler
(`dxcompiler.dll`, `dxil.dll`, from its official releases on
github.com/microsoft/DirectXShaderCompiler), and openRTS may load and use
them at run time, even though they are not under the GPL. Those binaries
remain under their own terms (`licenses/dxc` in the release archive); this
permission does not extend the GPL to them nor their terms to openRTS.

openRTS works without them: Direct3D 12 then compiles the shaders with the
system's FXC, much slower, and Vulkan does not use them at all.

## NVIDIA DLSS (Streamline)

You may convey openRTS, and works based on it, together with the
unmodified distributable binaries of NVIDIA Streamline and DLSS
(`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`,
from the official Streamline SDK releases on github.com/NVIDIA-RTX/Streamline),
and openRTS may load and use them at run time, even though they are not
under the GPL. Those binaries remain under their own terms (the SDK's
`license.txt` and `nvngx_dlss.license.txt`); this permission does not
extend the GPL to them nor their terms to openRTS.

openRTS works without them: DLAA is then not offered, and its own
temporal anti-aliasing stays.

If you modify openRTS you may extend these permissions to your version,
but you are not obliged to; you may delete them from your version.

---

openRTS распространяется под GNU GPL версии 3 (`LICENSE`). По её разделу 7
правообладатели openRTS дают следующее дополнительное разрешение.

## Компилятор шейдеров Microsoft DirectX

Разрешается распространять openRTS и основанные на нём работы вместе с
неизменёнными распространяемыми файлами Microsoft DirectX Shader Compiler
(`dxcompiler.dll`, `dxil.dll` из его официальных выпусков на
github.com/microsoft/DirectXShaderCompiler), а openRTS — загружать и
использовать их во время работы, хотя они не под GPL. Эти файлы остаются
под своими условиями (`licenses/dxc` в архиве выпуска); разрешение не
распространяет GPL на них, а их условия — на openRTS.

openRTS работает и без них: тогда Direct3D 12 собирает шейдеры системным
FXC, гораздо медленнее, а Vulkan их вовсе не использует.

## NVIDIA DLSS (Streamline)

Разрешается распространять openRTS и основанные на нём работы вместе с
неизменёнными распространяемыми файлами NVIDIA Streamline и DLSS
(`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` из
официальных выпусков Streamline SDK на github.com/NVIDIA-RTX/Streamline), а
openRTS — загружать и использовать их во время работы, хотя они не под GPL.
Эти файлы остаются под своими условиями (`license.txt` и
`nvngx_dlss.license.txt` из SDK); разрешение не распространяет GPL на них,
а их условия — на openRTS.

openRTS работает и без них: тогда DLAA не предлагается, а остаётся своё
временное сглаживание.

Изменяя openRTS, вы можете распространить эти разрешения на свою версию,
но не обязаны; можете убрать их из своей версии.
