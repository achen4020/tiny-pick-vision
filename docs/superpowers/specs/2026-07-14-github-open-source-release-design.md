# Tiny Pick Vision GitHub 开源发布设计

日期：2026-07-14
状态：已确认

## 1. 目标

将现有 GitHub 仓库 `achen4020/tiny-pick-vision` 在保留完整开发历史的前提下转为公开仓库，并补齐首次开源所需的许可证、项目说明、第三方声明和发布检查。

公开版本需要让新用户能够准确理解项目边界、完成本地构建与测试，并清楚区分当前已实现能力和规划中的 SDK 能力。

## 2. 已确认决策

- 继续使用现有 GitHub 仓库，不创建新的公开仓库。
- 保留完整 Git 历史，但重写全部提交的 Author 和 Committer 邮箱为 `der20044@msn.com`。
- 保留原作者名称、提交时间和提交内容。
- 项目采用 Apache License 2.0。
- 版权署名使用 `Copyright 2026 Alvin Chen`。
- README 使用中英双语。
- 保留 `android/app/src/main/assets/blaze_face_short_range.tflite`，同时补充模型来源和第三方许可证声明。
- 当前工作区已经完成的 Review 修复纳入首次公开版本。

## 3. 发布方案

采用“发布前完整整理”方案：先保持仓库私有，完成内容整理、历史重写和验证，再强制推送重写后的历史，最后将仓库切换为 Public。

不采用以下方案：

- 最小发布：仅添加 README 和 LICENSE。该方案无法解决历史邮箱及第三方模型声明问题。
- 新建干净仓库：该方案会丢失现有开发历史，不符合本次目标。

## 4. 开源文件

### 4.1 README.md

README 采用中文在前、英文在后的双语结构，包含：

- 项目定位和适用场景；
- C SDK、Android Bench App、工具链的架构关系；
- 当前能力和明确限制；
- 仓库目录结构；
- 主机端构建和测试；
- 标定工具工作流；
- Android Demo 构建要求与运行方式；
- 回放、时延和 size gate 验证；
- 隐私与生物特征数据说明；
- 路线图和贡献入口；
- 许可证与第三方声明链接。

README 必须明确：

- Object 模式当前使用 TPV 阈值、连通域、形状特征和分类管线，不是通用神经网络目标检测；
- Face 模式当前使用 Android MediaPipe Face Detector，只做人脸检测和跟踪，不做身份识别；
- Android MediaPipe 实现是 Demo/Spike，第三方产品能力最终必须通过 C ABI 提供；
- `src/model_data.c` 由标定工具生成且不进入 Git，克隆仓库后部分目标需要先完成标定。

### 4.2 LICENSE

添加标准 Apache License 2.0 正文，不改写许可证条款。

### 4.3 NOTICE

添加项目版权通知：

```text
Tiny Pick Vision
Copyright 2026 Alvin Chen
```

### 4.4 THIRD_PARTY_NOTICES.md

记录至少以下第三方内容：

- MediaPipe Tasks Vision Android 依赖；
- 仓库内的 BlazeFace short-range TFLite 模型；
- Gradle Wrapper；
- 其他随源码或二进制一同再分发、且需要保留声明的组件。

每项声明包括组件名称、用途、版本或文件路径、上游项目链接、许可证类型及许可证链接。对于模型文件，同时记录 SHA-256，避免后续替换模型后来源说明失效。

## 5. Git 历史处理

历史重写覆盖现有仓库可达的全部分支和标签：

- 将所有提交的 Author 邮箱统一为 `der20044@msn.com`；
- 将所有提交的 Committer 邮箱统一为 `der20044@msn.com`；
- 不改作者名称、提交者名称、提交时间、提交消息和文件内容；
- 重写后扫描全部提交，确认旧邮箱不再出现；
- 记录重写前的本地备份引用，完成验证前不删除。

历史重写会改变全部相关提交 SHA。远程更新必须使用带租约保护的强制推送，并在仓库仍为私有时完成。

## 6. 发布安全检查

公开前执行：

- 当前文件和完整 Git 历史的敏感信息扫描；
- 大文件、构建产物、设备采集文件和本地配置检查；
- `.gitignore` 检查；
- 作者和提交者邮箱复查；
- 模型文件 SHA-256 与第三方声明核对；
- `git diff --check`；
- C 测试、标定工具测试、Android JVM 测试、APK 构建；
- ARM ABI/layout 和 20 KB size gate；
- APK 中模型 SHA 校验。

若发现凭证、私有数据、授权不明确的二进制或设备采集内容，发布暂停，先移除或明确授权。

## 7. GitHub 发布步骤

1. 修复 GitHub CLI 登录状态并确认目标账号为 `achen4020`。
2. 确认远程仓库当前为 Private，避免在准备过程中提前公开。
3. 提交 Review 修复和开源材料。
4. 创建重写前备份引用。
5. 重写历史邮箱并执行历史复查。
6. 运行完整发布验证。
7. 使用 `--force-with-lease` 更新远程分支和相关标签。
8. 将 `achen4020/tiny-pick-vision` 可见性切换为 Public。
9. 设置仓库描述、Homepage（如有）和 Topics。
10. 从未登录视角检查 README、LICENSE、文件下载和 clone 流程。

建议 Topics：

```text
computer-vision, embedded-vision, android, camerax, c-sdk,
object-detection, face-detection, tracking, robotics, tflite
```

## 8. 失败处理

- GitHub 登录失败：不执行推送或可见性修改，保留本地成果并请求用户重新认证。
- 历史重写验证失败：不推送，回到备份引用重新执行。
- 测试、APK、size gate 或 SHA 校验失败：仓库保持 Private，修复后重新运行完整验证。
- 强制推送被租约保护拒绝：重新获取远程状态并检查差异，不绕过保护直接覆盖。
- 第三方模型授权无法确认：仓库保持 Private，改为移除模型并提供官方下载步骤。

## 9. 完成标准

满足以下全部条件才算发布完成：

- GitHub 仓库为 Public；
- 未登录用户可以访问和克隆；
- README 中英文内容完整且命令经过验证；
- GitHub 正确识别 Apache-2.0；
- NOTICE 和第三方声明存在；
- 全部历史提交邮箱已统一为 `der20044@msn.com`；
- 当前工作区 Review 修复已进入公开分支；
- 敏感信息扫描无未处理发现；
- C、Android、APK、ABI、size 和 SHA 验证全部通过；
- 模型文件来源、许可证和 SHA 已记录。
