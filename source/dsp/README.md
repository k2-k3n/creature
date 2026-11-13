# Creature Voice

处理 Wah 和 Growl。按住旋钮时扫过 Wah 频段，松手时回落。

## 使用方法

1. 先在 Resona Box 上安装本音频包。
2. 安装并打开 Creature 0.5.3。
3. 在 App 界面使用上方和下方编码器控制音色与功能。

## 配套关系

- 音频包：`org.resona.demo.creature.voice`，版本 0.2.4
- App：`org.resona.demo.creature`，版本 0.5.3

完整界面及双编码器的旋转、短按、长按说明见 Creature 的社区项目页面。

DRY 现在直接对比未经滤波与限幅的吉他输入。

Wah 系数每 16 个采样更新一次，采样计数跨音频处理块连续。

松手直接弹也有四种音色。MOSS 圆润，VELVET 偏暗，CINDER 明亮，REED 带人声感。按住下方仍能扫 Wah，短按上方切换 DRY。
