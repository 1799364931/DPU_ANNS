# 程序框架设计审阅任务

## 目标
阅读 ANNS 中现有全部 Markdown 文档，提出模块、目录与核心对象设计，等待用户审阅后才能编写代码。

## 阶段
- [x] 清点文档和现有实现状态。
- [x] 阅读全部既有 Markdown 文档并提取约束。
- [x] 形成程序设计框架，核对协议与实现计划。
- [x] 交付设计文档供用户审阅。

## 范围
只新增本任务的 Markdown 规划与设计文档，不实现程序、不安装依赖、不运行实验、不提交或推送。

## 环境问题
- 默认沙箱因 /mnt/wslg/distro 挂载异常无法启动；只读与授权的文档写入使用 require_escalated。
- 工作目录不是 Git 仓库，git status 不可用。
- python 命令不可用；改用已存在的 /usr/bin/python3 更新 Markdown，无需安装。

## 状态
设计已完成，交付 ANNS/program_design_v1.md，等待用户审阅后再进入代码编写。现有 ANNS/planning/task_plan.md 属于上一轮实施规划，保留原样。
