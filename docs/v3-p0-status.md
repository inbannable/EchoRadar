# v3 P0：进行中，未通过可行性验收

2026-09-12。本阶段保留 v2 作为测量工具；它不是新的 v3 产品。
完整重写、Qt UI/HUD、音频桥及依赖路线选择的算法接入尚未开始。
用户补充偏好：**优先 Sonic Radar 类方向方法，非必要不使用方向模型**。
多声道优先公开频谱能量方法；双耳先测传统特征与真实 CS2 模板。
只有独立测试证明传统方案不够，才考虑独立方向模型，不能默认训练。

## 已有证据

- [v2 基线与归档](v2-archive.md)：最终 main 为 `9b603c6`，PR #6 已包含收尾。
- [端点枚举原始输出](validation/v3-p0/endpoints.txt)：Senary 耳机为
  2ch / 48 kHz；CABLE Input 为 8ch / 48 kHz，角色为
  FL, FR, FC, LFE, BL, BR, SL, SR。名为 CABLE In 16ch 的端点实际为 2ch。
- 默认耳机端点短时静音旁路 smoke 达到 Running，无报告丢帧；不证明有声录制、空间准确率或实际听音延迟。
- 检查时 CS2 未运行。已发现的 `ml/generated/v3-smoke-sessions` 样例标有
  `generator_version: mixture-v3` / `renderer: steam-audio-v4.8.1`，不能用于真实 CS2 验收。
  尚未确定可用的人工方向标注实录目录；未修改这些数据。
- 当前 `models/v4-candidate` 五个文件的
  [SHA-256](validation/v3-p0/frozen-model-files.json) 及
  [原始参数副本](validation/v3-p0/frozen-model-metadata.json) 已冻结记录。
  枪声 quiet/busy 阈值原本都是 1.0，保留原值，不为了改善报告而更改。

## 可独立运行的 P0 工具

本次工具回归：13/13 通过，原始输出见
[tool-tests.txt](validation/v3-p0/tool-tests.txt)。测试使用构造信号，只验证工具行为，
不计入真实 CS2 验收。提交前复核五个冻结模型文件 SHA-256 全部未变。

工具位于 `tools/v3_p0`，使用现有 `ml/.venv/Scripts/python.exe` 和 NumPy。
所有输出 create-only，已有文件不会覆盖。没有播放、注入、游戏坐标读取或训练。

```powershell
# 校验 PCM16 WAV；支持 WAVEFORMATEXTENSIBLE，保留声明的声道角色
& ml/.venv/Scripts/python.exe -B tools/v3_p0/p0.py audit recordings/v3-p0/session.wav --output recordings/v3-p0/audit.json

# 离线因果特征；不输出脚步/枪声置信度，不将立体声映射成环绕箭头
& ml/.venv/Scripts/python.exe -B tools/v3_p0/features.py recordings/v3-p0/session.wav --output recordings/v3-p0/features.json

# 人工完成 manifest 后，检查录音哈希、格式、角色、事件范围和 dev/test 泄漏
& ml/.venv/Scripts/python.exe -B tools/v3_p0/p0.py validate recordings/v3-p0/manifest.json --output recordings/v3-p0/manifest-check.json

# 每种冻结算法独立导出 predictions；在相同测试集比较
& ml/.venv/Scripts/python.exe -B tools/v3_p0/p0.py evaluate recordings/v3-p0/manifest.json recordings/v3-p0/predictions.json --split test --output recordings/v3-p0/metrics.json

& ml/.venv/Scripts/python.exe -B -m unittest discover -s tools/v3_p0 -p 'test_*.py' -v
```

`audit` 输出逐通道 RMS、峰值、削波比例、相关矩阵和协方差秩。
它们只能发现静音、复制等线索；高秩也可能来自升混、混响或驱动处理。
没有任何自动“CS2 原始 7.1 已通过”的判据。

`features` 是 P0 离线研究探针：1024 点 Hann、480 点 hop，六个通用分频段，
因果背景跟踪和正向能量变化。双耳输出分频 ILD、限制到 ±1ms 的 GCC-PHAT
时差、八帧平滑相干性；无实录模板时拒判方向。最初七帧不报告相干性。
多声道按声明角色计算逐频点能量向量及 24 扇区，排除 LFE，保留多个局部峰。
不同频率的相反来源可以形成两个峰；同频相反来源仍可抵消。
所有窗长、频段、扇区数和一致性阈值均为 EchoRadar 未校准研究参数，非 ASUS 参数。
这些特征不直接构成脚步/枪声分类，也不是最终产品算法的验收结果。

`baselines.py` 实现可复现的“纯频段”和“频段＋时间”候选事件对比入口。
频段方案在能量/SNR 越过阈值时输出一次候选；时间方案增加起音变化、
持续时间范围和可配置重复节奏，需要等该段结束，因此必须单独测延迟。
不把规则响应转换成概率（分类置信度为 null），不把整个混音的方向峰
分配给某个类别。事件方向关联未验证时明确拒判。
没有提供假称已校准的脚步/枪声频段和阈值：配置必须由真实 dev 集校准后保存。

配置 JSON 包含 `schema: 1`、`calibrated_on: "dev"`、
`development_manifest_sha256`、`calibration_notes`，以及
`rules.footstep` 和 `rules.gunshot`。每个规则包含：
`band_indices`（上面六个频段的零基索引）、`minimum_energy`、
`minimum_snr_db`、`minimum_flux_ratio`、`minimum_duration_ms`、
`maximum_duration_ms`、`minimum_repeats`、`repeat_min_ms`、`repeat_max_ms`。
哈希与声明提供追溯信息，不能自动证明校准过程没有查看测试集。

```powershell
& ml/.venv/Scripts/python.exe -B tools/v3_p0/baselines.py recordings/v3-p0/manifest.json recordings/v3-p0/rules.json --mode frequency --split test --output recordings/v3-p0/frequency-predictions.json
& ml/.venv/Scripts/python.exe -B tools/v3_p0/baselines.py recordings/v3-p0/manifest.json recordings/v3-p0/rules.json --mode frequency-time --split test --output recordings/v3-p0/temporal-predictions.json
```

两种方案必须使用相同冻结配置与相同测试录音；旧模型对照尚待接入相同实录评测。

评测器用类别和 ±100ms 起音做最大匹配数的一对一匹配，匹配过程不查看方向。
额外同类预测计 FP，缺失预测计 FN，方向拒判降低覆盖率；缺失测量为 null。
报告类别指标、方向分位数、覆盖率、前后混淆分母、全部真值的联合成功比例。
前后混淆约定排除真值距正侧面 5° 内的事件，必须保留分母并在正式验收前冻结约定。
HUD 时间必须来自与录音对齐的实际呈现测量；离线处理完成时间不能填入该字段。
旁路/转发听音延迟需另行硬件测量。工具不会自动宣告验收通过。

## 需要实际操作的下一步

先采用直接耳机路线。用户需启动 CS2 离线受控场景，固定视角，记录实际地图、
音频设置及驱动增强状态。准备另一个可控声音来源，分别在前、右、后、左产生
脚步与枪声，初轮隔离事件，随后增加同时脚步/枪声、遮挡、混响和玩家转身。
不要把自身脚步或自身开枪自动标成固定外部方向。

每个场次独立录制短文件，使用不同文件名；先执行端点列表确认 ID 仍有效。
以下使用已构建、已测试的 **v2 测量工具**，不改变游戏输出设备、不重新播放音频：

```powershell
New-Item -ItemType Directory -Force recordings/v3-p0 | Out-Null
& build/tools/audio_monitor/Release/audio_monitor.exe --list-devices
# 确认目标文件不存在；v2 工具本身可能覆盖目标文件
if (Test-Path recordings/v3-p0/dev-mapA-session01.wav) { throw '请选择新文件名' }
& build/tools/audio_monitor/Release/audio_monitor.exe --audio-output-id ma:a1a86832f7803d6a --record recordings/v3-p0/dev-mapA-session01.wav --seconds 30
```

记录期间不要切设备、休眠或更改格式。保留终端输出；任何断流、代次变化、
丢帧或 backlog discard 都使该场次无效，重新录制。v2 工具没有完善的硬件时间戳
证据包，所以本轮只能用于短时内容/特征研究，不用于端到端延迟证明。

复制 [manifest 模板](v3-p0-manifest.example.json) 到 recordings 下填写。
模板默认 `source: unverified` / `reviewed: false`，故意不能通过实录校验。
听审录音并用同步视频/受控场景记录标注每个事件相对于 WAV 首样本的起音毫秒，
以玩家视角正前为 0°、右为 90°、后为 180°、左为 270°。转身时标签必须跟随
事件当时视角，不能沿用旧世界方向。无法确定方向设 `localizable: false`。
非目标声音保留在录音中；其误检仍计 FP。空事件清单仅可用于人工确认的负样本。

不同地图和物理录制场次拆 dev/test；同文件裁剪不得伪造新的录制场次以跨集合。
先用 dev 校准，再冻结配置，独立 test 不调参。测试集同时需要负样本。
[预测模板](v3-p0-predictions.example.json) 中数字为格式示例，不是模型输出。

多声道路线还需逐声道回录和 CS2 受控方向证据。音频桥未完成前不将 CS2 切至
虚拟设备作为日常耳机链路；不启用“侦听此设备”冒充低延迟方案。

## 尚未完成的门槛

| 项目 | 状态 / 所需证据 |
| --- | --- |
| 实际 CS2 独立空间输入 | 未验证；需要逐声道及真实受控方向录制 |
| 纯频段 vs 频段+时间 vs 冻结旧模型 | 待真实 dev/test 数据；没有校准/对比结果 |
| 双耳模板方向 | 待真实方向录制；未训练方向模型 |
| 分类、方向、关联及覆盖率 | 未测量，不存在通过结果 |
| 播放/HUD/转发附加延迟 | 未测量 |
| 两小时、设备恢复、DPI/HUD | 未执行 |

P0 未关闭，不能依此推进依赖结果的 P1/P2 产品路线或声称 v3 重构完成。
完整要求及 P1–P4 验收保持在 [v3 计划](v3-plan.md)，没有因当前缺数据而删减。
