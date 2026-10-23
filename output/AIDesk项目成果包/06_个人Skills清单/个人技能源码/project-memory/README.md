# project-memory Skill

用于先沟通项目需求和技术栈，通过后建立项目记忆库，再按编号任务开发并同步文档。

## 调用

在安装该 Skill 的项目中调用：

```text
/project-memory 我想做一个库存管理系统，先帮我讨论需求与技术选型。
```

继续已有项目：

```text
/project-memory 继续当前项目，先读取记忆库并确认本次任务。
```

此 Skill 仅在用户主动调用时加载。安装后如会话尚未发现新技能，重新打开项目会话再调用。

## 安装

将整个 project-memory 文件夹放到以下任一位置，保留内部目录结构：

- Claude Code 项目级：<项目根目录>/.claude/skills/project-memory/
- Claude Code 用户级：~/.claude/skills/project-memory/，用于不同项目。
- 支持 SKILL.md 的其他工具：放到该工具规定的技能目录；自动发现和调用方式以该工具为准。

必须包含 SKILL.md、references/ 和 assets/；不要只复制 SKILL.md。

## 工作流程

1. 澄清项目目标、用户、核心需求、期望效果、平台和约束。
2. 讨论技术栈，整理需求、验收标准和编号任务。
3. 等待用户明确通过；未回复不代表通过。
4. 建立或合并九份记忆文档，加入项目协作规则。
5. 每次执行一个已批准任务，验证后同步文档和状态。

新项目默认使用 memory_bank/。已有 memory-bank/ 时沿用，不建立两套记忆库。

## 文件

- SKILL.md：技能入口与完整工作流程。
- references/document-templates.md：九份记忆文档模板与一致性检查。
- references/source-analysis.md：OwnerWeb 来源文件职责及通用化说明。
- assets/AGENTS.template.md：项目协作规则模板。

模板在实际调用、用户确认项目方案后才展开到目标项目。创建此技能不会替 OwnerWeb 重建记忆库或修改业务代码。
