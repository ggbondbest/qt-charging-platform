# 充电平台 Git 协作指令

本文供五名组员日常开发时直接查阅。项目采用：

```text
master  <-  develop  <-  feature/<area>-<topic>
稳定版本     集成分支       个人任务分支
```

## 1. 先选对协作方式

### 1.1 推荐方式：Collaborator 直接协作

组员接受本仓库的 Collaborator 邀请后，个人仓库协作者已经具有写权限，因此：

- 不需要 Fork；
- 不需要添加 `upstream`；
- `origin` 直接指向团队仓库；
- 每人只向自己的 `feature/...` 分支推送；
- 通过 Pull Request 合入 `develop`；
- 即使拥有写权限，也不得直接向 `develop` 或 `master` 推送。

团队仓库地址：

```text
https://github.com/ggbondbest/qt-charging-platform.git
```

这是本项目五人协作应统一使用的方式。

### 1.2 备用方式：Fork 协作

只有没有加入 Collaborators、无法向团队仓库推送分支时，才需要 Fork：

- `origin`：自己的 Fork；
- `upstream`：团队原仓库；
- 功能分支推送到自己的 Fork；
- 再向团队仓库的 `develop` 创建 Pull Request。

两套方式选择一套即可，不要把它们的命令混在一起。

## 2. 常用名称和命令是什么意思

| 名称或命令 | 作用 |
| --- | --- |
| 工作区 | 当前实际编辑的文件 |
| 暂存区 | 本次准备提交的文件集合 |
| 本地仓库 | 当前电脑里的 Git 提交和分支 |
| 远程仓库 | GitHub 上的仓库 |
| `origin` | `git clone` 自动创建的远程仓库简称 |
| `upstream` | Fork 模式下手动添加的团队原仓库简称 |
| `git clone` | 第一次把远程仓库完整下载到本地 |
| `git status` | 查看当前分支、修改和暂存状态 |
| `git switch` | 切换或创建分支，推荐使用 |
| `git checkout` | 较旧的切换分支写法，本项目也可以使用 |
| `git fetch` | 下载远程新提交，但暂不修改当前分支 |
| `git pull` | 下载远程提交并合入当前分支 |
| `git add` | 把修改加入暂存区 |
| `git commit` | 把暂存区内容保存为本地提交 |
| `git push` | 把本地提交上传到远程分支 |
| `git merge` | 把另一条分支的历史合入当前分支 |
| `git rebase` | 把当前分支的提交重新接到另一提交之后 |

一组修改从代码到 GitHub 的方向是：

```text
编辑文件 -> git add -> git commit -> git push -> Pull Request -> Review -> develop
```

## 3. Collaborator 推荐流程

### 3.1 每台开发环境首次配置

姓名和邮箱会记录在 Commit 中。每台开发环境只需配置一次：

```bash
git config --global user.name "你的 GitHub 用户名"
git config --global user.email "你的 GitHub 邮箱"
```

检查配置：

```bash
git config --global user.name
git config --global user.email
```

### 3.2 第一次下载项目

先在 GitHub 接受 Collaborator 邀请，然后执行：

```bash
git clone https://github.com/ggbondbest/qt-charging-platform.git
cd qt-charging-platform
git switch develop
git pull --ff-only origin develop
```

如果习惯 `checkout`，下面两条切换命令含义相同：

```bash
git switch develop
git checkout develop
```

检查远程地址：

```bash
git remote -v
```

Collaborator 模式下，输出中的 `origin` 应当指向：

```text
https://github.com/ggbondbest/qt-charging-platform.git
```

### 3.3 每个新任务都从最新 develop 建分支

开始前先确认没有遗漏的本地修改：

```bash
git status
```

然后更新 `develop` 并创建任务分支：

```bash
git switch develop
git pull --ff-only origin develop
git switch -c feature/client-station-list
```

对应的旧式 `checkout` 写法是：

```bash
git checkout develop
git pull --ff-only origin develop
git checkout -b feature/client-station-list
```

`--ff-only` 可以防止一次普通的 `pull` 意外在本地 `develop` 上生成 Merge Commit。

分支名使用小写英文和连字符，例如：

```text
feature/client-login-ui
feature/client-station-list
feature/client-profile
feature/server-dashboard
feature/database-order-repository
feature/network-charging-status
```

一个分支只对应一个任务。不要从另一个人的 feature 分支开始新任务，也不要五个人共用一个长期开发分支。

### 3.4 查看、暂存和提交修改

写完一小段可独立说明的代码后，先检查修改：

```bash
git status
git diff
```

优先按文件添加，不要无条件添加所有文件：

```bash
git add client/pages/station_page.cpp
git add client/pages/station_page.h
```

确定所有修改都属于本次提交时，也可以使用：

```bash
git add .
```

提交前再次检查暂存内容：

```bash
git diff --cached
```

创建 Commit：

```bash
git commit -m "feat(client): add station list page"
```

常用提交类型：

| 类型 | 用途 | 示例 |
| --- | --- | --- |
| `feat` | 新增功能 | `feat(client): add station list page` |
| `fix` | 修复缺陷 | `fix(network): handle disconnected socket` |
| `ui` | 仅修改界面或样式 | `ui(server): add dashboard cards` |
| `refactor` | 重构且不改变外部行为 | `refactor(database): split query helpers` |
| `test` | 测试 | `test(auth): cover invalid phone number` |
| `docs` | 文档 | `docs(team): add Git workflow guide` |
| `build` | CMake 或构建配置 | `build(client): link Qt Network` |
| `ci` | 持续集成配置 | `ci: verify Qt 6.2.4 build` |
| `chore` | 其他维护工作 | `chore: update ignore rules` |

不要提交 `build/`、运行时 SQLite 数据库、密码、Token、Qt Creator 用户配置或临时文件。

### 3.5 第一次推送任务分支

```bash
git push -u origin feature/client-station-list
```

`-u` 会让本地分支记住对应的远程分支。这个分支后续继续提交时只需：

```bash
git push
```

如果 GitHub 提示没有权限，依次检查：

1. 是否已经接受 Collaborator 邀请；
2. `git remote -v` 的 `origin` 是否为团队仓库；
3. 当前 Git 登录账号是否为受邀请的账号；
4. 是否错误地直接推送了受保护的 `develop` 或 `master`。

### 3.6 创建 Pull Request

推送后，在 GitHub 创建 Pull Request：

```text
base:    develop
compare: feature/client-station-list
```

然后：

1. 填写 PR 模板和实际测试结果；
2. 等待 `Ubuntu 22.04 / Qt 6.2.4` CI 通过；
3. 邀请至少一名非作者组员 Review；
4. 修复阻塞意见；
5. 按团队约定使用 `Squash and merge` 合并。

PR 创建后，如果继续向同一 feature 分支 `commit` 和 `push`，原 PR 会自动更新，不需要重新创建。

## 4. 开发中同步最新 develop

当其他人的 PR 已合入 `develop` 时，自己的分支可能需要同步。同步前先执行：

```bash
git status
```

如果当前修改尚未完成，优先先完成一个合理 Commit。确实暂时无法提交时，可以临时保存：

```bash
git stash push -u -m "wip before syncing develop"
```

### 4.1 推荐给初学者：merge

在自己的 feature 分支上执行：

```bash
git fetch origin --prune
git switch feature/client-station-list
git merge origin/develop
```

没有冲突时直接推送：

```bash
git push
```

发生冲突时：

```bash
git status
```

打开冲突文件，保留最终需要的代码，并删除以七个小于号开头的
`<<<<<<< 当前分支`、中间的 `=======` 和以七个大于号开头的
`>>>>>>> 合入分支` 三类冲突标记。

处理完成后：

```bash
git add 冲突文件路径
git commit
git push
```

如果不确定并希望取消本次合并：

```bash
git merge --abort
```

### 4.2 可选方式：rebase

`rebase` 会让提交历史更直，但会改写 feature 分支的提交历史。只有在理解它的影响，并且该分支由自己独立使用时才执行：

```bash
git fetch origin --prune
git switch feature/client-station-list
git rebase origin/develop
```

发生冲突时：

```bash
git status
# 编辑并解决冲突
git add 冲突文件路径
git rebase --continue
```

如果还有冲突，重复“编辑、`git add`、`git rebase --continue`”。放弃本次变基：

```bash
git rebase --abort
```

如果该 feature 分支以前没有推送过，正常推送：

```bash
git push -u origin feature/client-station-list
```

如果以前已经推送过，因为 `rebase` 改写了历史，需要：

```bash
git push --force-with-lease origin feature/client-station-list
```

只能对自己独立使用的 feature 分支使用 `--force-with-lease`。绝对不要对 `develop`、`master` 或多人共用分支强制推送，也不要把它换成风险更大的 `--force`。

### 4.3 恢复临时保存的修改

如果同步前使用了 `stash`，同步成功后回到原分支并恢复：

```bash
git switch feature/client-station-list
git stash pop
```

恢复时也可能出现冲突，应通过 `git status` 查看并逐个处理。

## 5. PR 合并后清理分支

先在 GitHub 确认 PR 已经成功合并，然后：

```bash
git switch develop
git pull --ff-only origin develop
git fetch origin --prune
git branch -d feature/client-station-list
```

如果使用 `Squash and merge`，Git 可能认为原 feature 提交没有直接合入，从而拒绝 `-d`。只有在确认 PR 已合并、功能代码已经进入 `develop` 且分支不再需要后，才能执行：

```bash
git branch -D feature/client-station-list
```

如果 GitHub 没有自动删除远程分支：

```bash
git push origin --delete feature/client-station-list
```

下一个任务重新从最新 `develop` 建立新分支，不要继续使用已经合并的旧 feature 分支。

## 6. 常见检查和纠错命令

### 6.1 查看当前分支和所有本地分支

```bash
git branch --show-current
git branch
```

### 6.2 查看简洁提交历史

```bash
git log --oneline --decorate --graph -20
```

### 6.3 文件加错暂存区，但保留文件修改

```bash
git restore --staged 文件路径
```

### 6.4 查看远程仓库和对应分支

```bash
git remote -v
git branch -vv
```

### 6.5 查看保存的 stash

```bash
git stash list
```

不要在不理解影响时使用 `git reset --hard`、`git clean -fd`、`git push --force` 或随意删除分支。这些命令可能导致未提交内容或远程历史丢失。

## 7. 未加入 Collaborators 时的 Fork 流程

本节只是备用流程。已接受 Collaborator 邀请的五名成员跳过本节。

### 7.1 第一次配置 Fork

先在 GitHub 页面点击 `Fork`，然后克隆自己的仓库：

```bash
git clone https://github.com/你的用户名/qt-charging-platform.git
cd qt-charging-platform
git remote add upstream https://github.com/ggbondbest/qt-charging-platform.git
git remote -v
```

此时应该是：

```text
origin    -> 自己的 Fork
upstream  -> 团队原仓库
```

获取团队的 `develop`：

```bash
git fetch upstream --prune
```

如果本地还没有 `develop`，第一次执行：

```bash
git switch -c develop --track upstream/develop
```

如果本地已经有 `develop`，执行：

```bash
git switch develop
git pull --ff-only upstream develop
```

### 7.2 Fork 模式下开发和推送

```bash
git switch develop
git pull --ff-only upstream develop
git switch -c feature/client-station-list

# 编写代码后
git status
git add 指定文件路径
git diff --cached
git commit -m "feat(client): add station list page"
git push -u origin feature/client-station-list
```

然后在 GitHub 创建以下 PR：

```text
base repository:  ggbondbest/qt-charging-platform
base branch:      develop
head repository: 你的用户名/qt-charging-platform
compare branch:   feature/client-station-list
```

Fork 模式下，`git pull --ff-only upstream develop` 是为了从团队仓库同步；Collaborator 模式没有 `upstream`，使用的是 `git pull --ff-only origin develop`。

## 8. 每天最常用的命令清单

新任务开始：

```bash
git status
git switch develop
git pull --ff-only origin develop
git switch -c feature/模块名-功能名
```

完成一部分代码：

```bash
git status
git diff
git add 指定文件路径
git diff --cached
git commit -m "feat(module): concise description"
git push -u origin feature/模块名-功能名
```

同一分支后续提交：

```bash
git status
git add 指定文件路径
git commit -m "fix(module): concise description"
git push
```

PR 合并后：

```bash
git switch develop
git pull --ff-only origin develop
git fetch origin --prune
git branch -d feature/模块名-功能名
```

## 9. 本项目最终约定

1. 五名成员接受 Collaborator 邀请后，统一采用直接协作，不再 Fork。
2. 直接协作只有 `origin`，不需要 `upstream`。
3. 每个任务从最新 `develop` 创建独立 feature 分支。
4. 使用 `git add`、`git commit` 和 `git push` 把修改推到自己的 feature 分支。
5. 通过 PR、CI 和非作者 Review 合入 `develop`。
6. 初学阶段优先用 `merge` 同步 `develop`；熟悉后可在自己的分支使用 `rebase`。
7. `master` 和 `develop` 均禁止直接开发、直接推送和强制推送。

相关资料：

- [GitHub：邀请个人仓库协作者](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/repository-access-and-collaboration/inviting-collaborators-to-a-personal-repository)
- [Git：使用远程仓库](https://git-scm.com/book/zh/v2/Git-%e5%9f%ba%e7%a1%80-%e8%bf%9c%e7%a8%8b%e4%bb%93%e5%ba%93%e7%9a%84%e4%bd%bf%e7%94%a8)
- [Git：变基](https://git-scm.com/book/zh/v2/Git-%e5%88%86%e6%94%af-%e5%8f%98%e5%9f%ba)
