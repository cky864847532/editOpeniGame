# iGameVis 扁平化图标集（2.5D 轻量）

依据 `Qt/Resources/Icons` 下的 3D 拟物图标（渐变 + 高光 + 投影，512×512 PNG）重绘的一版**扁平图标**。
保留原有隐喻与命名，去掉高光/投影/渐变，改为 2D 线面结合风格，适合深色与浅色两套主题。

## 文件

- `*.svg`：60 个图标，文件名与原 PNG **完全同名**（如 `StreamTracer.svg` ↔ `StreamTracer.png`），可直接替换。
- `preview.html`：预览页（深/浅底 + 24/32/48px 三种尺寸）。
- `compare.html`：**新旧对比页**（左原版 3D / 右新版扁平），支持：
  - 尺寸滑杆 16~64px；
  - 背景：深色 / 浅色 / 棋盘；
  - 模式：并排 / 叠加（拖动「叠加位置」做前后对比）；
  - 分组筛选（文件与输出、可视化、视图、坐标轴、播放、模型树/选择、通用）；
  - 搜索框按名称过滤；点击卡片可弹窗放大到 24/48/96px 对比。
  - 也支持 URL 参数，例如：`compare.html?mode=overlay&bg=light&size=48`。
- `_preview.png` / `_compare.png`：两个页面的截图。

> 两个 HTML 用相对路径读取原图标（`../../Icons/…`），请保持本目录结构，直接双击用浏览器打开即可。

未重绘：`iGameLogo.png`（品牌 Logo，保持原样）。

## 设计规范

| 项 | 取值 |
|---|---|
| 画布 | `viewBox="0 0 24 24"`，四周留 2~3px 安全边距 |
| 线宽 | 主形 1.5~1.9px，强调 2px |
| 线帽/转角 | `stroke-linecap="round"` + `stroke-linejoin="round"` |
| 圆角 | 元素圆角 1~2px，整体不描外框 |
| 立体感 | 仅用「两种明度的面」表达体块（2.5D），不使用渐变、高光、投影 |

## 配色

| 用途 | 色值 |
|---|---|
| 主色（蓝） | `#4C9BFF` |
| 主色暗部 | `#2E5F9E` |
| 中性（板岩灰） | `#A9B7C9` |
| 中性暗部 | `#6B7A90` |
| 强调红 | `#F0736E` |
| 强调绿 | `#5FCF8C` |
| 强调琥珀 | `#E9B44C` |
| 面板底色（徽标衬底） | `#16233F` |

> 主色 `#4C9BFF` 与当前「石墨深色 / 深灰蓝」主题的强调蓝一致；灰阶在深色底与浅色底上均可辨。

## 命名对照（原 PNG → 新 SVG）

文件/输出：`Open`、`SaveFile`、`ScreenShot`、`animation_save_vcr`、`Delete`

可视化：`StreamTracer`、`Glyph`、`Deformation`、`Slice`、`Contourline`、`Chart`、`Chart-1`、`VolumeRendering`、`ScalarBar`

视图：`IsometricView`、`orthographic`、`ShowOrientationAxes`、`ResetCenter`、`PickCenter`、`ShowCenterAxes`、`ResetScaleRange`、`SetScaleRange`、`BackGround`、`refresh`、`clear`

坐标轴：`+X`、`-X`、`+Y`、`-Y`、`+Z`、`-Z`、`youxuanzhuan90du`（顺时针 90°）、`zuoxuanzhuan90du`（逆时针 90°）

播放控制：`VcrPlay`、`VcrPause`、`VcrReverse`、`VcrFirst`、`VcrLast`、`VcrBack`、`VcrForward`、`VcrLoop`、`VcrDisabledLoop`、`skip-previous`

模型树 / 选择：`Eyeball`、`EyeballClosed`、`eye-open`、`eye-close`、`bbox`、`point`、`points`、`fill`、`wireframe`、`hex`、`selected`、`select_face`、`select_vertex_geodesic`、`selectView`、`DragPoint`、`ContextPreservingAction`、`check`

## 在 Qt 工程中使用

1. 把需要的 SVG 放进 `Qt/Resources/Icons/`（或保持在本目录）。
2. 在 `iGameQtMainWindow.qrc` 里按原 PNG 的路径添加对应 `.svg` 条目。
3. 代码/界面里把 `QIcon(":/Ticon/Icons/xxx.png")` 换成 `QIcon(":/Ticon/Icons/xxx.svg")`。

> 工程已部署 Qt5Svg（`qsvgd.dll` / `qsvgicond.dll`），无需额外依赖；
> SVG 为矢量，工具栏图标缩放时不会像 PNG 那样发虚。

## 预览

浏览器打开 `preview.html` 即可查看全部图标在深/浅背景、24/32/48px 下的效果。
