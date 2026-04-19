# BGI-Hazuki ♡ 解封包工具

<div align="center">
  <img src="static/bg.png" width="400" alt="BGI-Hazuki Background" />
</div>

> 面向 BGI / Ethornell / BURIKO ARC20 游戏资源的 Windows 解包处理工具。

> 小濑叶月可爱捏~妈妈！！！！

**[English →](README.md)**

---

> ⚠️ **免责声明与警告**  
> 本项目仅供技术研究、逆向学习、个人存档与非商业 Mod 制作使用。  
> 通过本工具提取出的游戏文本、图像、音频及其他资源，其版权与知识产权均归原开发商与发行方所有。  
> 严禁将本工具用于商业传播、大规模盗版或其他侵权用途；由使用行为引发的一切后果由使用者自行承担。

---

## 1. 包含哪些程序

当前项目面向最终用户提供五个主要二进制文件：

- `BGI_Hazuki_GUI.exe` - 图形界面，负责拖入 `.arc` 后统一解包。
- `BGI_Hazuki_Core.dll` - GUI 依赖的原生核心库。
- `BGI_Hazuki_ImageTool.exe` - 独立的 PNG <-> CBG 转换工具。
- `BGI_Hazuki_TextTool.exe` - 独立的 DSC 文本提取 / 回写工具。
- `BGI_Hazuki_AssetTool.exe` - 独立的资源探测 / 解出工具。

另外还保留了一个可选的旧工具：

- `BGI_Unpacker.exe` - 早期 Python 版 ARC 拖拽解包器，仅用于兼容老习惯。

如果你只想使用 GUI，真正必须放在一起的只有两个文件：

- `BGI_Hazuki_GUI.exe`
- `BGI_Hazuki_Core.dll`

这两个文件必须位于同一目录。

---

## 2. 运行要求

正常使用只需要：

1. Windows
2. 游戏使用 BGI / Ethornell 引擎

一般情况下不需要额外安装第三方运行库。

---

## 3. 文件放在哪里

推荐把 `BGI_Hazuki_GUI.exe` 和 `BGI_Hazuki_Core.dll` 直接放到游戏根目录，也就是和需要处理的 `.arc` 文件处于同一层。

示意：

```text
放学后的灰姑娘2\
├── data01000.arc
├── system.arc
├── sysprg.arc
├── ...
├── BGI_Hazuki_GUI.exe
└── BGI_Hazuki_Core.dll
```

放好后直接双击 `BGI_Hazuki_GUI.exe` 即可。

---

## 4. GUI 快速使用

1. 启动 `BGI_Hazuki_GUI.exe`。
2. 把一个或多个 `.arc` 文件拖进窗口，或点击 **选择文件**。
3. 等待状态变为完成。
4. 点击 **打开输出目录** 直接查看结果。

默认输出位置为当前工作目录下的：

```text
unpack\<arc_name>\
```

GUI 在 ARC 解包完成后，会继续自动处理以下内容：

- `CBG 图像` -> `PNG`
- `DSC / 编译脚本` -> `.hazuki.txt`
- `bw 音频` -> `OGG`

如果你想脚本化批处理，也可以直接命令行调用 GUI：

```powershell
BGI_Hazuki_GUI.exe system.arc sysprg.arc --auto-close
```

适合无人值守批量解包。

---

## 5. 输出文件保留规则

现在的输出目录只尽量保留对后续编辑或回写真正有意义的文件。

- CBG 在成功导出 PNG 后，会删除原始无扩展名中间文件。
- DSC / 编译脚本原文件会保留，因为文本回写时仍然需要原始脚本作为模板。
- `bw` 音频原文件会保留，因为重新封装时仍然需要原始容器头。

可以简单理解为：

- 只有 `PNG` 也能重新编码回 `CBG`。
- 只有 `.hazuki.txt` 不能单独回写成脚本，仍然需要原始脚本文件。
- 只有 `OGG` 不能单独重建 BGI 音频，仍然需要原始 `bw` 文件。

---

## 6. 独立工具说明

### `BGI_Hazuki_ImageTool.exe`

支持直接拖入 PNG、CBG 或整个目录。

- `PNG` -> `CBG`
- `CBG` -> `PNG`

### `BGI_Hazuki_TextTool.exe`

用法：

```text
BGI_Hazuki_TextTool extract <script> [output.hazuki.txt] [--decode-cp 932] [--encode-cp 932]
BGI_Hazuki_TextTool apply <script.hazuki.txt> [output_script] [--encode-cp 932]
```

也支持拖拽自动模式：

- 拖入 DSC 脚本时自动提取文本
- 拖入 `.hazuki.txt` 工程时自动回写

### `BGI_Hazuki_AssetTool.exe`

用法：

```text
BGI_Hazuki_AssetTool probe <file-or-dir> [...]
BGI_Hazuki_AssetTool unpack <file-or-dir> [...] [--decode-cp 932] [--encode-cp 932]
```

适合处理已经从 ARC 中解出的散文件。当前可自动识别：

- CBG 图像
- DSC 脚本 / 原始编译脚本
- BGI 音频（`bw  `）

### `BGI_Unpacker.exe`

这是旧版 ARC 专用拖拽解包器。它没有现在 GUI 的自动后处理流程，但作为兼容工具仍然保留。

---

## 7. 目录结构速览

```text
BGI_Hazuki/
├── bin/                 最终原生程序输出目录
├── BGI_Unpacker/        旧版 Python ARC 解包器及其生成 exe
├── include/             原生核心公开头文件
├── resources/           图标资源脚本
├── src/                 C++ 源码
├── static/              背景图与图标源文件
├── tools/               构建辅助脚本
├── build.bat            一键构建入口
├── clean.bat            清理脚本
├── README.md            英文说明
└── README.zh-CN.md      中文说明
```

---

## 8. 从源码构建

构建依赖：

- Visual Studio 2022，安装“使用 C++ 的桌面开发”
- Windows SDK
- 若要同时构建 `BGI_Unpacker.exe`，还需要带 `PyInstaller` 的 Python 环境

一键构建：

```powershell
.\build.bat
```

清理中间产物并保留最终程序：

```powershell
.\clean.bat
```

当前构建会生成：

- `bin\BGI_Hazuki_GUI.exe`
- `bin\BGI_Hazuki_Core.dll`
- `bin\BGI_Hazuki_ImageTool.exe`
- `bin\BGI_Hazuki_TextTool.exe`
- `bin\BGI_Hazuki_AssetTool.exe`
- `BGI_Unpacker\dist\BGI_Unpacker.exe`

---

## 9. 常见问题

**Q：双击 GUI 就闪退？**  
A：大概率是 `BGI_Hazuki_Core.dll` 没和 exe 放在同一目录。

**Q：为什么输出目录里还有一部分原始文件？**  
A：脚本和音频的回写流程仍然依赖原始二进制文件，所以它们会保留。

**Q：GUI 现在能不能把 ARC 再封回去？**  
A：还不能。当前 GUI 只做解包和自动分流；编辑与回写由独立工具负责。

**Q：脚本编码页应该填多少？**  
A：默认 `932`，也就是 Shift-JIS，对大多数 BGI 日文游戏是正确的。

---

Enjoy your patching! ✿ 若遇到 bug 欢迎反馈~