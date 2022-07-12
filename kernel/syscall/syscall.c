#include "syscall.h"
#include "../process/process.h"
#include <exception/gate.h>
#include <exception/irq.h>
#include <driver/disk/ahci/ahci.h>
#include <mm/slab.h>
#include <common/errno.h>
#include <common/fcntl.h>
#include <filesystem/fat32/fat32.h>
#include <filesystem/VFS/VFS.h>
#include <driver/keyboard/ps2_keyboard.h>
#include <process/process.h>
#include <time/sleep.h>

// 导出系统调用入口函数，定义在entry.S中
extern void system_call(void);
extern void syscall_int(void);

extern uint64_t sys_clock(struct pt_regs* regs);


/**
 * @brief 导出系统调用处理函数的符号
 *
 */
#define SYSCALL_COMMON(syscall_num, symbol) extern unsigned long symbol(struct pt_regs *regs);
SYSCALL_COMMON(0, system_call_not_exists); // 导出system_call_not_exists函数
#undef SYSCALL_COMMON                      // 取消前述宏定义

/**
 * @brief 重新定义为：把系统调用函数加入系统调用表
 * @param syscall_num 系统调用号
 * @param symbol 系统调用处理函数
 */
#define SYSCALL_COMMON(syscall_num, symbol) [syscall_num] = symbol,

/**
 * @brief sysenter的系统调用函数，从entry.S中跳转到这里
 *
 * @param regs 3特权级下的寄存器值,rax存储系统调用号
 * @return ul 对应的系统调用函数的地址
 */
ul system_call_function(struct pt_regs *regs)
{
    return system_call_table[regs->rax](regs);
}

/**
 * @brief 初始化系统调用模块
 *
 */
void syscall_init()
{
    kinfo("Initializing syscall...");

    set_system_trap_gate(0x80, 0, syscall_int); // 系统调用门
}

/**
 * @brief 通过中断进入系统调用
 *
 * @param syscall_id
 * @param arg0
 * @param arg1
 * @param arg2
 * @param arg3
 * @param arg4
 * @param arg5
 * @param arg6
 * @param arg7
 * @return long
 */

long enter_syscall_int(ul syscall_id, ul arg0, ul arg1, ul arg2, ul arg3, ul arg4, ul arg5, ul arg6, ul arg7)
{
    long err_code;
    __asm__ __volatile__(
        "movq %2, %%r8 \n\t"
        "movq %3, %%r9 \n\t"
        "movq %4, %%r10 \n\t"
        "movq %5, %%r11 \n\t"
        "movq %6, %%r12 \n\t"
        "movq %7, %%r13 \n\t"
        "movq %8, %%r14 \n\t"
        "movq %9, %%r15 \n\t"
        "int $0x80   \n\t"
        : "=a"(err_code)
        : "a"(syscall_id), "m"(arg0), "m"(arg1), "m"(arg2), "m"(arg3), "m"(arg4), "m"(arg5), "m"(arg6), "m"(arg7)
        : "memory", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rcx", "rdx");
    return err_code;
}

/**
 * @brief 打印字符串的系统调用
 *
 * 当arg1和arg2均为0时，打印黑底白字，否则按照指定的前景色和背景色来打印
 *
 * @param regs 寄存器
 * @param arg0 要打印的字符串
 * @param arg1 前景色
 * @param arg2 背景色
 * @return ul 返回值
 */
ul sys_put_string(struct pt_regs *regs)
{

    printk_color(regs->r9, regs->r10, (char *)regs->r8);
    // printk_color(BLACK, WHITE, (char *)regs->r8);

    return 0;
}

uint64_t sys_open(struct pt_regs *regs)
{
    char *filename = (char *)(regs->r8);
    int flags = (int)(regs->r9);
    // kdebug("filename=%s", filename);

    long path_len = strnlen_user(filename, PAGE_4K_SIZE) + 1;

    if (path_len <= 0) // 地址空间错误
    {
        return -EFAULT;
    }
    else if (path_len >= PAGE_4K_SIZE) // 名称过长
    {
        return -ENAMETOOLONG;
    }

    // 为待拷贝文件路径字符串分配内存空间
    char *path = (char *)kmalloc(path_len, 0);
    if (path == NULL)
        return -ENOMEM;
    memset(path, 0, path_len);

    strncpy_from_user(path, filename, path_len);
    // 去除末尾的 '/'
    if (path_len >= 2 && path[path_len - 2] == '/')
    {
        path[path_len - 2] = '\0';
        --path_len;
    }

    // 寻找文件
    struct vfs_dir_entry_t *dentry = vfs_path_walk(path, 0);

    // if (dentry != NULL)
    //     printk_color(ORANGE, BLACK, "Found %s\nDIR_FstClus:%#018lx\tDIR_FileSize:%#018lx\n", path, ((struct fat32_inode_info_t *)(dentry->dir_inode->private_inode_info))->first_clus, dentry->dir_inode->file_size);
    // else
    //     printk_color(ORANGE, BLACK, "Can`t find file\n");
    // kdebug("flags=%#018lx", flags);
    if (dentry == NULL && flags & O_CREAT)
    {
        // 先找到倒数第二级目录
        int tmp_index = -1;
        for (int i = path_len - 1; i >= 0; --i)
        {
            if (path[i] == '/')
            {
                tmp_index = i;
                break;
            }
        }

        struct vfs_dir_entry_t *parent_dentry = NULL;
        // kdebug("tmp_index=%d", tmp_index);
        if (tmp_index > 0)
        {
            
            path[tmp_index] = '\0';
            dentry = vfs_path_walk(path, 0);
            if (dentry == NULL)
            {
                kfree(path);
                return -ENOENT;
            }
            parent_dentry = dentry;
        }
        else
            parent_dentry = vfs_root_sb->root;

        // 创建新的文件
        dentry = (struct vfs_dir_entry_t *)kmalloc(sizeof(struct vfs_dir_entry_t), 0);
        memset(dentry, 0, sizeof(struct vfs_dir_entry_t));

        dentry->name_length = path_len - tmp_index - 1;
        dentry->name = (char *)kmalloc(dentry->name_length, 0);
        memset(dentry->name, 0, dentry->name_length);
        strncpy(dentry->name, path + tmp_index + 1, dentry->name_length);
        // kdebug("to create new file:%s   namelen=%d", dentry->name, dentry->name_length)
        dentry->parent = parent_dentry;
        uint64_t retval = parent_dentry->dir_inode->inode_ops->create(parent_dentry->dir_inode, dentry, 0);
        if (retval != 0)
        {
            kfree(dentry->name);
            kfree(dentry);
            kfree(path);
            return retval;
        }

        list_init(&dentry->child_node_list);
        list_init(&dentry->subdirs_list);
        list_add(&parent_dentry->subdirs_list, &dentry->child_node_list);
        // kdebug("created.");
    }
    kfree(path);
    if (dentry == NULL)
        return -ENOENT;

    // 要求打开文件夹而目标不是文件夹
    if ((flags & O_DIRECTORY) && (dentry->dir_inode->attribute != VFS_ATTR_DIR))
        return -ENOTDIR;

    // // 要找的目标是文件夹
    // if ((flags & O_DIRECTORY) && dentry->dir_inode->attribute == VFS_ATTR_DIR)
    //     return -EISDIR;

    // todo: 引入devfs后删除这段代码
    // 暂时遇到设备文件的话，就将其first clus设置为特定值
    if (path_len >= 5 && filename[0] == '/' && filename[1] == 'd' && filename[2] == 'e' && filename[3] == 'v' && filename[4] == '/')
    {
        if (dentry->dir_inode->attribute & VFS_ATTR_FILE)
        {
            // 对于fat32文件系统上面的设备文件，设置其起始扇区
            ((struct fat32_inode_info_t *)(dentry->dir_inode->private_inode_info))->first_clus |= 0xf0000000;
            dentry->dir_inode->sb->sb_ops->write_inode(dentry->dir_inode);
            dentry->dir_inode->attribute |= VFS_ATTR_DEVICE;
        }
    }

    // 创建文件描述符
    struct vfs_file_t *file_ptr = (struct vfs_file_t *)kmalloc(sizeof(struct vfs_file_t), 0);
    memset(file_ptr, 0, sizeof(struct vfs_file_t));

    int errcode = -1;

    file_ptr->dEntry = dentry;
    file_ptr->mode = flags;

    // todo: 接入devfs
    // 特判一下是否为键盘文件
    if (dentry->dir_inode->attribute & VFS_ATTR_DEVICE)
    {
        file_ptr->file_ops = &ps2_keyboard_fops; // 如果是设备文件，暂时认为它是键盘文件
    }
    else
        file_ptr->file_ops = dentry->dir_inode->file_ops;

    // 如果文件系统实现了打开文件的函数
    if (file_ptr->file_ops && file_ptr->file_ops->open)
        errcode = file_ptr->file_ops->open(dentry->dir_inode, file_ptr);

    if (errcode != VFS_SUCCESS)
    {
        kfree(file_ptr);
        return -EFAULT;
    }

    if (file_ptr->mode & O_TRUNC) // 清空文件
        file_ptr->dEntry->dir_inode->file_size = 0;

    if (file_ptr->mode & O_APPEND)
        file_ptr->position = file_ptr->dEntry->dir_inode->file_size;
    else
        file_ptr->position = 0;

    struct vfs_file_t **f = current_pcb->fds;

    int fd_num = -1;

    // 在指针数组中寻找空位
    // todo: 当pcb中的指针数组改为动态指针数组之后，需要更改这里（目前还是静态指针数组）
    for (int i = 0; i < PROC_MAX_FD_NUM; ++i)
    {
        if (f[i] == NULL) // 找到指针数组中的空位
        {
            fd_num = i;
            break;
        }
    }

    // 指针数组没有空位了
    if (fd_num == -1)
    {
        kfree(file_ptr);
        return -EMFILE;
    }
    // 保存文件描述符
    f[fd_num] = file_ptr;

    return fd_num;
}

/**
 * @brief 关闭文件系统调用
 *
 * @param fd_num 文件描述符号
 *
 * @param regs
 * @return uint64_t
 */
uint64_t sys_close(struct pt_regs *regs)
{
    int fd_num = (int)regs->r8;

    // kdebug("sys close: fd=%d", fd_num);
    // 校验文件描述符范围
    if (fd_num < 0 || fd_num > PROC_MAX_FD_NUM)
        return -EBADF;
    // 文件描述符不存在
    if (current_pcb->fds[fd_num] == NULL)
        return -EBADF;
    struct vfs_file_t *file_ptr = current_pcb->fds[fd_num];
    uint64_t ret;
    // If there is a valid close function
    if (file_ptr->file_ops && file_ptr->file_ops->close)
        ret = file_ptr->file_ops->close(file_ptr->dEntry->dir_inode, file_ptr);

    kfree(file_ptr);
    current_pcb->fds[fd_num] = NULL;
    return 0;
}

/**
 * @brief 从文件中读取数据
 *
 * @param fd_num regs->r8 文件描述符号
 * @param buf regs->r9 输出缓冲区
 * @param count regs->r10 要读取的字节数
 *
 * @return uint64_t
 */
uint64_t sys_read(struct pt_regs *regs)
{
    int fd_num = (int)regs->r8;
    void *buf = (void *)regs->r9;
    int64_t count = (int64_t)regs->r10;

    // kdebug("sys read: fd=%d", fd_num);

    // 校验文件描述符范围
    if (fd_num < 0 || fd_num > PROC_MAX_FD_NUM)
        return -EBADF;

    // 文件描述符不存在
    if (current_pcb->fds[fd_num] == NULL)
        return -EBADF;

    if (count < 0)
        return -EINVAL;

    struct vfs_file_t *file_ptr = current_pcb->fds[fd_num];
    uint64_t ret;
    if (file_ptr->file_ops && file_ptr->file_ops->read)
        ret = file_ptr->file_ops->read(file_ptr, (char *)buf, count, &(file_ptr->position));

    return ret;
}

/**
 * @brief 向文件写入数据
 *
 * @param fd_num regs->r8 文件描述符号
 * @param buf regs->r9 输入缓冲区
 * @param count regs->r10 要写入的字节数
 *
 * @return uint64_t
 */
uint64_t sys_write(struct pt_regs *regs)
{
    int fd_num = (int)regs->r8;
    void *buf = (void *)regs->r9;
    int64_t count = (int64_t)regs->r10;

    kdebug("sys write: fd=%d", fd_num);

    // 校验文件描述符范围
    if (fd_num < 0 || fd_num > PROC_MAX_FD_NUM)
        return -EBADF;

    // 文件描述符不存在
    if (current_pcb->fds[fd_num] == NULL)
        return -EBADF;

    if (count < 0)
        return -EINVAL;

    struct vfs_file_t *file_ptr = current_pcb->fds[fd_num];
    uint64_t ret;
    if (file_ptr->file_ops && file_ptr->file_ops->write)
        ret = file_ptr->file_ops->write(file_ptr, (char *)buf, count, &(file_ptr->position));

    return ret;
}

/**
 * @brief 调整文件的访问位置
 *
 * @param fd_num 文件描述符号
 * @param offset 偏移量
 * @param whence 调整模式
 * @return uint64_t 调整结束后的文件访问位置
 */
uint64_t sys_lseek(struct pt_regs *regs)
{
    int fd_num = (int)regs->r8;
    long offset = (long)regs->r9;
    int whence = (int)regs->r10;

    // kdebug("sys_lseek: fd=%d", fd_num);
    uint64_t retval = 0;

    // 校验文件描述符范围
    if (fd_num < 0 || fd_num > PROC_MAX_FD_NUM)
        return -EBADF;

    // 文件描述符不存在
    if (current_pcb->fds[fd_num] == NULL)
        return -EBADF;

    struct vfs_file_t *file_ptr = current_pcb->fds[fd_num];
    if (file_ptr->file_ops && file_ptr->file_ops->lseek)
        retval = file_ptr->file_ops->lseek(file_ptr, offset, whence);

    return retval;
}

uint64_t sys_fork(struct pt_regs *regs)
{
    // kdebug("sys_fork");
    return do_fork(regs, 0, regs->rsp, 0);
}
uint64_t sys_vfork(struct pt_regs *regs)
{
    kdebug("sys vfork");
    return do_fork(regs, CLONE_VM | CLONE_FS | CLONE_SIGNAL, regs->rsp, 0);
}

/**
 * @brief 将堆内存调整为arg0
 *
 * @param arg0 新的堆区域的结束地址
 * arg0=-1  ===> 返回堆区域的起始地址
 * arg0=-2  ===> 返回堆区域的结束地址
 * @return uint64_t 错误码
 *
 */
uint64_t sys_brk(struct pt_regs *regs)
{
    uint64_t new_brk = PAGE_2M_ALIGN(regs->r8);

    // kdebug("sys_brk input= %#010lx ,  new_brk= %#010lx bytes current_pcb->mm->brk_start=%#018lx current->end_brk=%#018lx", regs->r8, new_brk, current_pcb->mm->brk_start, current_pcb->mm->brk_end);

    if ((int64_t)regs->r8 == -1)
    {
        // kdebug("get brk_start=%#018lx", current_pcb->mm->brk_start);
        return current_pcb->mm->brk_start;
    }
    if ((int64_t)regs->r8 == -2)
    {
        // kdebug("get brk_end=%#018lx", current_pcb->mm->brk_end);
        return current_pcb->mm->brk_end;
    }
    if (new_brk > current_pcb->addr_limit) // 堆地址空间超过限制
        return -ENOMEM;

    int64_t offset;
    if (new_brk >= current_pcb->mm->brk_end)
        offset = (int64_t)(new_brk - current_pcb->mm->brk_end);
    else
        offset = -(int64_t)(current_pcb->mm->brk_end - new_brk);

    

    new_brk = mm_do_brk(current_pcb->mm->brk_end, offset); // 扩展堆内存空间

    current_pcb->mm->brk_end = new_brk;
    return 0;
}

/**
 * @brief 将堆内存空间加上offset（注意，该系统调用只应在普通进程中调用，而不能是内核线程）
 *
 * @param arg0 offset偏移量
 * @return uint64_t the previous program break
 */
uint64_t sys_sbrk(struct pt_regs *regs)
{
    uint64_t retval = current_pcb->mm->brk_end;
    if ((int64_t)regs->r8 > 0)
    {

        uint64_t new_brk = PAGE_2M_ALIGN(retval + regs->r8);
        if (new_brk > current_pcb->addr_limit) // 堆地址空间超过限制
        {
            kdebug("exceed mem limit, new_brk = %#018lx", new_brk);
            return -ENOMEM;
        }
    }
    else
    {
        if ((__int128_t)current_pcb->mm->brk_end + (__int128_t)regs->r8 < current_pcb->mm->brk_start)
            return retval;
    }
    // kdebug("do brk");
    uint64_t new_brk = mm_do_brk(current_pcb->mm->brk_end, (int64_t)regs->r8); // 调整堆内存空间
    // kdebug("do brk done, new_brk = %#018lx", new_brk);
    current_pcb->mm->brk_end = new_brk;
    return retval;
}

/**
 * @brief 重启计算机
 *
 * @return
 */
uint64_t sys_reboot(struct pt_regs *regs)
{
    // 重启计算机
    io_out8(0x64, 0xfe);

    return 0;
}

/**
 * @brief 切换工作目录
 *
 * @param dest_path 目标路径
 * @return
+--------------+------------------------+
|    返回码    |          描述          |
+--------------+------------------------+
|      0       |          成功          |
|   EACCESS    |        权限不足        |
|    ELOOP     | 解析path时遇到路径循环 |
| ENAMETOOLONG |       路径名过长       |
|    ENOENT    |  目标文件或目录不存在  |
|    ENODIR    |  检索期间发现非目录项  |
|    ENOMEM    |      系统内存不足      |
|    EFAULT    |       错误的地址       |
| ENAMETOOLONG |        路径过长        |
+--------------+------------------------+
 */
uint64_t sys_chdir(struct pt_regs *regs)
{
    char *dest_path = (char *)regs->r8;
    // kdebug("dest_path=%s", dest_path);
    // 检查目标路径是否为NULL
    if (dest_path == NULL)
        return -EFAULT;

    // 计算输入的路径长度
    int dest_path_len;
    if (regs->cs & USER_CS)
    {
        dest_path_len = strnlen_user(dest_path, PAGE_4K_SIZE);
    }
    else
        dest_path_len = strnlen(dest_path, PAGE_4K_SIZE);

    // 长度小于等于0
    if (dest_path_len <= 0)
        return -EFAULT;
    else if (dest_path_len >= PAGE_4K_SIZE)
        return -ENAMETOOLONG;

    // 为路径字符串申请空间
    char *path = kmalloc(dest_path_len + 1, 0);
    // 系统内存不足
    if (path == NULL)
        return -ENOMEM;

    memset(path, 0, dest_path_len + 1);
    if (regs->cs & USER_CS)
    {
        // 将字符串从用户空间拷贝进来, +1是为了拷贝结尾的\0
        strncpy_from_user(path, dest_path, dest_path_len + 1);
    }
    else
        strncpy(path, dest_path, dest_path_len + 1);
    // kdebug("chdir: path = %s", path);
    struct vfs_dir_entry_t *dentry = vfs_path_walk(path, 0);

    kfree(path);

    if (dentry == NULL)
        return -ENOENT;
    // kdebug("dentry->name=%s, namelen=%d", dentry->name, dentry->name_length);
    // 目标不是目录
    if (dentry->dir_inode->attribute != VFS_ATTR_DIR)
        return -ENOTDIR;

    return 0;
}

/**
 * @brief 获取目录中的数据
 *
 * @param fd 文件描述符号
 * @return uint64_t
 */
uint64_t sys_getdents(struct pt_regs *regs)
{
    int fd = (int)regs->r8;
    void *dirent = (void *)regs->r9;
    long count = (long)regs->r10;

    if (fd < 0 || fd > PROC_MAX_FD_NUM)
        return -EBADF;

    if (count < 0)
        return -EINVAL;

    struct vfs_file_t *filp = current_pcb->fds[fd];
    if (filp == NULL)
        return -EBADF;

    uint64_t retval = 0;
    if (filp->file_ops && filp->file_ops->readdir)
        retval = filp->file_ops->readdir(filp, dirent, &vfs_fill_dentry);

    return retval;
}

/**
 * @brief 执行新的程序
 *
 * @param user_path(r8寄存器) 文件路径
 * @param argv(r9寄存器) 参数列表
 * @return uint64_t
 */
uint64_t sys_execve(struct pt_regs *regs)
{
    // kdebug("sys_execve");
    char *user_path = (char *)regs->r8;
    char **argv = (char **)regs->r9;

    int path_len = strnlen_user(user_path, PAGE_4K_SIZE);

    // kdebug("path_len=%d", path_len);
    if (path_len >= PAGE_4K_SIZE)
        return -ENAMETOOLONG;
    else if (path_len <= 0)
        return -EFAULT;

    char *path = (char *)kmalloc(path_len + 1, 0);
    if (path == NULL)
        return -ENOMEM;

    memset(path, 0, path_len + 1);

    // kdebug("before copy file path from user");
    // 拷贝文件路径
    strncpy_from_user(path, user_path, path_len);
    path[path_len] = '\0';

    // kdebug("before do_execve, path = %s", path);
    // 执行新的程序
    uint64_t retval = do_execve(regs, path, argv, NULL);

    kfree(path);
    return retval;
}

/**
 * @brief 等待进程退出
 *
 * @param pid 目标进程id
 * @param status 返回的状态信息
 * @param options 等待选项
 * @param rusage
 * @return uint64_t
 */
uint64_t sys_wait4(struct pt_regs *regs)
{
    uint64_t pid = regs->r8;
    int *status = (int *)regs->r9;
    int options = regs->r10;
    void *rusage = (void *)regs->r11;

    struct process_control_block *proc = NULL;
    struct process_control_block *child_proc = NULL;

    // 查找pid为指定值的进程
    // ps: 这里判断子进程的方法没有按照posix 2008来写。
    // todo: 根据进程树判断是否为当前进程的子进程
    for (proc = &initial_proc_union.pcb; proc->next_pcb != &initial_proc_union.pcb; proc = proc->next_pcb)
    {
        if (proc->next_pcb->pid == pid)
        {
            child_proc = proc->next_pcb;
            break;
        }
    }

    if (child_proc == NULL)
        return -ECHILD;

    // 暂时不支持options选项，该值目前必须为0
    if (options != 0)
        return -EINVAL;

    // 如果子进程没有退出，则等待其退出
    while (child_proc->state != PROC_ZOMBIE)
        wait_queue_sleep_on_interriptible(&current_pcb->wait_child_proc_exit);

    // 拷贝子进程的返回码
    *status = child_proc->exit_code;
    // copy_to_user(status, (void*)child_proc->exit_code, sizeof(int));
    proc->next_pcb = child_proc->next_pcb;

    // 释放子进程的页表
    process_exit_mm(child_proc);
    // 释放子进程的pcb
    kfree(child_proc);
    return 0;
}

/**
 * @brief 进程退出
 *
 * @param exit_code 退出返回码
 * @return uint64_t
 */
uint64_t sys_exit(struct pt_regs *regs)
{
    return process_do_exit(regs->r8);
}


uint64_t sys_nanosleep(struct pt_regs * regs)
{
    const struct timespec * rqtp = (const struct timespec*)regs->r8;
    struct timespec * rmtp = (struct timespec*)regs->r9;

    return nanosleep(rqtp, rmtp);
}

ul sys_ahci_end_req(struct pt_regs *regs)
{
    ahci_end_request();
    return 0;
}

// 系统调用的内核入口程序
void do_syscall_int(struct pt_regs *regs, unsigned long error_code)
{

    ul ret = system_call_table[regs->rax](regs);
    regs->rax = ret; // 返回码
}

system_call_t system_call_table[MAX_SYSTEM_CALL_NUM] =
    {
        [0] = system_call_not_exists,
        [1] = sys_put_string,
        [2] = sys_open,
        [3] = sys_close,
        [4] = sys_read,
        [5] = sys_write,
        [6] = sys_lseek,
        [7] = sys_fork,
        [8] = sys_vfork,
        [9] = sys_brk,
        [10] = sys_sbrk,
        [11] = sys_reboot,
        [12] = sys_chdir,
        [13] = sys_getdents,
        [14] = sys_execve,
        [15] = sys_wait4,
        [16] = sys_exit,
        [17] = sys_mkdir,
        [18] = sys_nanosleep,
        [19] = sys_clock,
        [20 ... 254] = system_call_not_exists,
        [255] = sys_ahci_end_req};
