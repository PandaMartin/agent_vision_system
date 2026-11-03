// =============================================================================
// sd_io —— SD 卡(FatFs)基础读写服务
//
// FatFs单线程纪律: 本模块全部函数只允许app_task调用; GUI侧经appctl命令+
// bridge快照间接访问(builtin/files应用即此范式)
// =============================================================================
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//目录项(名字按容量截断补0)
typedef struct {
    char    name[28];
    uint8_t is_dir;
    uint32_t size;         //字节(目录项无意义)
} SdDirEntry;

//SD应用条目(app_task装配: 枚举/apps/*.bin后逐个读头校验的载体;
//头解析在main.cpp——sd层保持通用, 不含AppHeader知识)
typedef struct {
    char name[16];         //AppHeader.name(截断补0)
    char file[32];         //文件名(/apps/下, 截断补0)
    uint32_t fw_version;   //应用头里的固件版本(mkapp封包时刻的导出版本)
    uint8_t ver_ok;        //版本与当前固件导出匹配(不匹配拒载, 桌面置灰)
} SdAppInfo;

//挂载 SD 卡(挂到 FatFs 根路径 ""), 返回 0 成功
int sd_mount(void);

//SD卡是否已挂载
int sd_mounted(void);

//读整个文件到 malloc 缓冲区(故意不 free: 模型等数据需长期驻留)
//返回 NULL 失败; 成功时 *out_size = 文件字节数
unsigned char* sd_read_file(const char* path, unsigned long* out_size);

//列举目录(app_task上下文): 填out[0..max), 目录在前文件在后各按名排序
//*n=实际条数; 总数超过max时*truncated=1(超出部分不返回)
//返回0成功; -1未挂载; -2打开目录失败(不存在/读错)
int sd_list_dir(const char* path, SdDirEntry* out, int max,
                int* n, int* truncated);

//读文件指定偏移起的len字节(app_task上下文)
//返回0成功; -1未挂载; -2打开失败; -3读取不完整(含偏移越界)
int sd_read_bytes(const char* path, uint32_t offset, void* dst, uint32_t len);

//写文件(创建/覆盖, app_task上下文; 串口收文件存SD用)
//返回0成功; -1未挂载; -2打开失败; -3写入不完整
int sd_write_file(const char* path, const uint8_t* data, uint32_t len);

//删除文件或目录(app_task上下文): 目录递归删除其下全部内容后移除,
//非空目录也支持(深度上限8层)。返回0成功; -1未挂载; -2删除失败
//(不存在/只读/超深层/路径超长, 失败时已删部分不回滚)
int sd_delete(const char* path);

//复制文件(app_task上下文): 分块读src写dst; dst重名时自动加"_N"序号后缀
//(实际落盘名打印到控制台); 中途失败删除半截目标文件不留垃圾
//返回0成功; -1未挂载; -2原地自拷拒绝; -3源打开失败; -4目标创建失败; -5读写失败
int sd_copy_file(const char* src, const char* dst);

//建目录(app_task上下文; 已存在不算错, 用于首次拍照前建 /photos)
//返回0成功(含已存在); -1未挂载; -2创建失败
int sd_mkdir(const char* path);

//逐级建目录(path如"0:/models/yolo", 途中已存在的层级跳过;
//app_task上下文, 网口推送任意路径文件前建父目录用)
//返回0成功(含全部已存在); -1未挂载; -2某一级创建失败
int sd_mkdirs(const char* path);

#ifdef __cplusplus
}
#endif
