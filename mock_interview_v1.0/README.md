# 模拟面试硬件 Agent v1.0

这是一个面向 ESP32-S3 + GC2145 摄像头硬件的语音模拟面试 MVP。它把一块带麦克风、扬声器、LCD 和摄像头的设备，连接到电脑端的 Agent Host，再接入大模型服务，形成一套可重复的面试训练闭环。

## 1. 项目用途

用户先说出面试主题，Agent 自动生成两道测试问题并通过扬声器播报。用户按住硬件 BOOT 键回答，硬件上传 PCM 音频，Host 调用 ASR 转写。面试过程中 GC2145 每 2.5 秒采集一帧画面并缓存；最后一题结束后，系统停止采集，分别生成回答质量总结和表情/肢体趋势总结，合并成一段简短语音播报。

本包当前默认是 2 题测试模式，便于快速验证完整链路。将来可把网页端 `maxQuestions=2` 改为随机 3 到 5 题。

## 2. 项目与应用创新

### 把面试训练从“聊天”变成“可完成的一场面试”

产品不是让用户随意和 AI 对话，而是复现真实面试的完整节奏：确认主题、听题、作答、结束后复盘。题目、回答和视觉表现属于同一场 session，最终得到一份完整结论，适合反复训练和对比进步。

### 同时评价“说了什么”和“怎么表现”

传统练习通常只看文字答案。本项目把回答内容、相关性、准确性、完整性、表达清晰度、语气词、回答时长，与表情自然度、动作幅度和紧张趋势放在同一份复盘中，形成更接近真实面试官的综合评价。

### 用连续变化而不是单张照片判断临场状态

面试时持续采集 GC2145 画面，结束后按时间顺序分析多帧趋势。系统关注“是否逐渐紧张”“动作是否越来越多”等变化，避免因为某一个瞬间眨眼、转头或表情停顿就下结论。

### 用原回答举例，让建议可以被验证

最终总结会引用候选人实际说过的短句，例如指出某个回答缺少数据、结论或结构，再给出下一次可执行的改法。用户能明确知道问题在哪里，而不是只得到“表达需要提升”这类泛化评价。

### 低干扰的硬件交互，更接近真实训练场景

用户只需听扬声器提问并按住 BOOT 说话，不需要在回答过程中频繁操作网页。LCD 持续显示本人画面，帮助用户建立镜头感；面试结束后才统一播报反馈，避免即时评分打断思路。

### 从求职面试扩展到多种表达训练

同一套流程可迁移到技术面、产品面、英语口语、演讲排练、答辩模拟、客服培训和销售话术训练。只需替换主题和评价维度，就能形成面向不同岗位或课程的专用训练器。

### 技术实现服务于产品体验

ESP32-S3、GC2145、BLE、ASR、TTS 和大模型是实现手段，不是项目价值本身。它们共同支撑“硬件采集、自然交互、整场复盘、可执行改进”这一核心体验；协议和模块细节请见后文架构章节。

## 3. 系统架构

```text
GC2145 + Mic + BOOT + LCD + Speaker
              |
          ESP32-S3 BLE
              |
       Web Bluetooth 页面
              |
       Python Agent Host
        |       |       |
       ASR   DeepSeek  TTS
              |
       Alibaba Qwen-VL
```

目录说明：

- `firmware/source/boards/assistant/`：GC2145、ST7789、ES8311、BOOT 和 LED 的板级实现。
- `firmware/source/components/agent_link/`：Agent Link 协议、BLE GATT、语音和 JPEG 分片传输。
- `firmware/source/main/`：ESP-IDF 入口和 Board 抽象。
- `firmware/build/agent_link.bin`：已经构建好的 ESP32-S3 固件。
- `firmware/flash/flash_args`：固件烧录地址参数。
- `host-python/host.py`：本地 Host，提供 `/asr`、`/tts`、`/interview`、`/vision/trend` 接口。
- `web/`：浏览器 UI、Web Bluetooth 连接、录音状态机、图像重组和最终总结展示。

## 4. BLE 通道

| 通道 | 方向 | 用途 |
|---|---|---|
| `0xFFA1` | 设备 -> 网页 | PCM 语音分片，供 ASR 使用 |
| `0xFFA2` | 网页 -> 设备 | TTS PCM，播放到扬声器 |
| `0xFFA3` | 设备 -> 网页 | JPEG 图像分片 |
| `0xFFC1` | 网页 -> 设备 | Agent Link 控制命令 |
| `0xFFC4` | 设备 -> 网页 | 控制事件和图像起止元数据 |

`camera0` 是板端注册的拍照端点。网页每 2.5 秒发送一次 `0x33` actuate 命令，板端捕获当前 GC2145 帧并编码为 JPEG。

## 5. 云端配置

复制 `host-python/.env.example` 为 `host-python/.env`，填入自己的密钥。不要把真实 `.env` 提交到 Git 或分享给他人。

当前验证过的配置形态：

```env
DEEPSEEK_API_KEY=...
DASHSCOPE_API_KEY=...
ASR_PROVIDER=dashscope
ASR_MODEL=paraformer-realtime-v2
TTS_PROVIDER=tencent
TENCENT_SECRET_ID=...
TENCENT_SECRET_KEY=...
VISION_PROVIDER=dashscope
VISION_MODEL=qwen-vl-plus
```

Host 需要能访问阿里云、腾讯云和 DeepSeek 的 HTTPS/WebSocket 服务。Windows 下若出现 `WinError 5` 或 `10013`，通常是进程网络权限或安全软件拦截，应在允许联网的终端中重新启动 Host。

## 6. 启动与测试

### 启动 Host

```powershell
cd D:\Hackthon\交付包\mock_interview_v1.0\host-python
python host.py
```

Host 地址：`http://127.0.0.1:8765`

### 启动网页

```powershell
cd D:\Hackthon\交付包\mock_interview_v1.0\web
python -m http.server 8080
```

打开：`http://localhost:8080/`

### 烧录固件

板子进入下载模式后，确认端口为 `COM7`，在 ESP-IDF 环境中执行：

```powershell
cd D:\Hackthon\fw_v1_ascii
idf.py -p COM7 flash
```

也可以使用本包中的 `firmware/build/agent_link.bin`，按照 `firmware/flash/flash_args` 配合 `esptool.py` 烧录。

### 完整操作流程

1. 给板子供电，打开网页。
2. 点击连接，选择 `AI_ASSISTANT`。
3. UI 显示 `请确认主题`，按住 BOOT 说出主题后松开。
4. 主题确认后，GC2145 自动开始每 2.5 秒采集；UI 显示 `第 1题/2题`。
5. 问题播报完成，UI 立即显示 `请按键回答`。
6. 按住 BOOT 回答，松开后等待 ASR 完成。
7. 第二题重复一次。
8. 第二题结束后自动停止照片采集，生成回答质量和照片趋势的综合分析，并通过扬声器简短播报。
9. 播报完成后自动清空本场数据，回到 `请确认主题`，可直接开始下一场。

## 7. 串口验证关键词

正常启动应看到：

```text
Detected GC2145 camera
camera ready (RGB565 240x240)
preview loop started
BLE started
```

正常采集应看到：

```text
snapshot requested (App)
snapshot: 240x240 -> ... jpeg, send=ESP_OK
image stream ... ended (status=0 valid_bytes=...)
```

`status=0` 且 `valid_bytes` 接近 JPEG 总长度，表示整张照片已传输完成。若固定停在较小字节数，通常是 BLE 通知拥塞或网页未订阅 `0xFFA3`。

## 8. 当前边界

- 当前固件和网页默认是 2 题测试模式。
- 视觉分析是面试结束后的多帧趋势总结，不是实时医疗或心理诊断。
- 视觉图片会上传到配置的阿里云视觉模型，使用时应告知参与者并遵守隐私和数据合规要求。
- BLE 图像仍受浏览器 GATT 通知吞吐限制，当前通过 180 字节分片、重试和 10ms 间隔提高稳定性。
- Host 是本地 MVP 服务，未包含账号系统、持久化数据库、任务队列或生产级鉴权。

## 9. 后续扩展

1. 将题目数量切换为主题级随机 3 到 5 题。
2. 保存历史面试记录和趋势曲线。
3. 用人体关键点模型替代纯视觉语言模型，获得更稳定的动作量化指标。
4. 增加 Wi-Fi/WebSocket 传输，降低 BLE 图像吞吐限制。
5. 为 Host 增加鉴权、日志脱敏、重试队列和部署配置。

## 10. 许可证与安全

本交付包不包含任何真实 API 密钥。请仅在本地 `.env` 中配置密钥，并在共享或提交前确认其未被打包。固件和 Agent Link 源码沿用原项目许可证；第三方 Espressif 组件保留其各自许可证声明。
