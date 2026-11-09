# Windows Q8 CUDA 部署

这个分支为 Windows 增加了原生 CUDA 构建脚本，以及 HTTP 和 WebSocket 多 Key 鉴权。

## 许可证

代码采用 Apache 2.0，可以 fork、修改和再分发。修改过的源码包含变更说明，原有许可证继续保留。MP3 编码器子模块 shine 采用 LGPL 2.0，随子模块保留其源码与许可证。

模型使用 BreezeBlue Research and Non-Commercial License。Q8 量化模型继续受模型许可证约束。个人研究、评估和非商业用途按该许可证执行，商业用途和商业 API 服务需要另行取得书面授权。当前部署按本机个人评估配置。

- [项目代码许可证](https://github.com/HoppouAI/Breeze-TTS-2.cpp/blob/main/LICENSE)
- [模型许可证](https://huggingface.co/BreezeBlue/Breeze-TTS-2/blob/main/LICENSE)

## 启动

在项目目录双击 `start_api_auth.cmd`。等待控制台出现 `backend: CUDA0`、预热完成及 `listening on` 消息。

| 入口 | HTTP | WebSocket |
| --- | --- | --- |
| `start_api_auth.cmd` | `127.0.0.1:7863` | `127.0.0.1:7864` |
| `start_api_original.cmd` | `127.0.0.1:7865` | `127.0.0.1:7866` |

WebUI 位于 `http://127.0.0.1:7863/`。在 API KEY 输入框填写一个有效密钥，再按 Tab。密钥只保留在当前页面内存中。Swagger 位于 `http://127.0.0.1:7863/docs`，点击 Authorize 填写密钥。Swagger 的前端资源从 jsDelivr 加载。

按 Ctrl+C 停止服务。旧的 `D:\Apps\breeze-tts` 可以继续独立使用。两份模型同时运行会叠加显存占用。

```powershell
.\start_api_auth.cmd -Port 7863 -WsPort 7864 -Parallel 1
.\start_api_auth.cmd -BindAddress 0.0.0.0 -Port 7863
.\start_api_auth.cmd -Help
```

启动脚本要求 CUDA 后端，默认启用 ggml CUDA Graphs，CUDA 架构默认针对本机 RTX 4070 Ti SUPER 的 SM 89。启动脚本会把 CUDA Toolkit 的 DLL 目录加入当前进程 PATH。

## 多 Key 配置

鉴权来源按下列优先级选择。

1. `BREEZE_API_KEYS` 环境变量，多个 Key 用逗号分隔。
2. `BREEZE_API_KEY` 环境变量，兼容单 Key。
3. `BREEZE_API_KEY_FILE` 环境变量指向的文件。
4. `-KeyFile` 指定的文件，默认项目根目录 `.env.api-key`。

文件每行一个 Key。支持 UTF-8、带 BOM 的 UTF-16 和 UTF-32。允许空行，Key 本身须为无空白的可打印 ASCII 字符。修改配置后重启服务。

本机部署时从原生项目复制了已有密钥到新项目的 `.env.api-key`。两份配置可各自修改。文件缺失时，鉴权启动脚本会生成随机 Key。凭据文件和模型文件已被 Git 忽略。

所有业务接口和健康检查都需要 `Authorization: Bearer <KEY>`。缺失、错误或重复 Authorization 头返回 401 和 `WWW-Authenticate: Bearer`。查询参数中的 Key 无效。文档和 WebUI 的静态页面公开访问。

WebSocket 客户端可在升级请求里发送相同的 Authorization 头。浏览器使用 `breeze-api-key.` 加上 Key 的无填充 base64url 编码作为子协议，内置 WebUI 会自动处理。

```powershell
$key = (Get-Content -LiteralPath .env.api-key | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
curl.exe --fail-with-body -H "Authorization: Bearer $key" http://127.0.0.1:7863/health
curl.exe --fail-with-body -H "Authorization: Bearer $key" http://127.0.0.1:7863/v1/model
```

## 排队与并行

HTTP 合成、声音转换、参考音频编码及 WebSocket 生成共用先进先出的调度队列。满载请求保持连接，等待空位后按进入队列的先后顺序开始。此规则在 `-Parallel 1` 和并行模式下都生效，同时处理数仍遵循 `-Parallel`。

CFG 请求在并行模式下占两个槽位，声音转换占全部槽位。队首需要多个槽位时，后续小请求也按顺序等待。排队中的 HTTP 断线、WebSocket 断线或取消消息会释放等待位置。生成或响应结束后，槽位交给后续请求。

`GET /v1/queue` 需要 Bearer Key，返回 `active`、`waiting` 和 `capacity`。`active` 是已占用槽位数，`waiting` 是等待请求数，`capacity` 是配置的并行槽位数。客户端超时应覆盖排队和合成时间。

`POST /v1/audio/speech` 接受现有 multipart 参数 `text`、`instruction`、`ref_audio`、`ref_text`、`cfg_scale`、`seed`。参考音频采用 WAV。原生项目已有 MP3 参考音频可先转换为 WAV。multipart 响应是 24 kHz 单声道 s16le PCM，OpenAI JSON 请求还支持 WAV 和 MP3。

## 获取模型和重建

模型固定为 `models/breeze-tts-2-q8_0.gguf`，大小 3,568,844,480 字节。SHA-256 为 `a02bcc4b69b0601032727f8040c4942149b1b73aa0f69022fe5aaa6a8f0ef879`。GGUF 包含运行所需的模型、词表和 codec 权重。

安装 Visual Studio C++ 工具和 NVIDIA CUDA Toolkit 后，在项目目录运行以下命令。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\download_q8.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_windows.ps1
```

下载脚本固定模型 revision 并校验 SHA-256，下载中断后可再次运行。构建脚本会生成 `build-cuda\breeze-server.exe` 和同目录运行库，并运行鉴权及排队回归测试。

若后续要让现有 FRP 指向这个服务，可在旧服务停止后用 `-Port 7861` 启动新鉴权服务，或修改 FRP 的 `localPort`。模型许可证中的对外服务条款需要同时遵守。
