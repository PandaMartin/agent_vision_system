// 提取界面文案用字: 扫描sw/app与main.cpp的字符串字面量中的CJK字符
// (在固件组件根运行: node tools/font/ui_chars.js > tools/font/ui_symbols.txt)
const fs = require('fs'), path = require('path');
const SRC = path.join(__dirname, '..', '..', 'src');
const set = new Set();
function scanFile(p) {
    const t = fs.readFileSync(p, 'utf8');
    const re = /"((?:[^"\\]|\\.)*)"/g;
    let m;
    while ((m = re.exec(t))) {
        for (const ch of m[1]) {
            const c = ch.codePointAt(0);
            if ((c >= 0x4e00 && c <= 0x9fff)   //CJK汉字
             || (c >= 0x3000 && c <= 0x303f)   //中文标点(。，、等)
             || (c >= 0x2010 && c <= 0x2027)   //破折号/引号/省略号…
             || (c >= 0x2190 && c <= 0x2193)   //箭头←↑→↓
             || (c >= 0xff01 && c <= 0xff60))  //全角！？：（）等
                set.add(ch);
        }
    }
}
function scan(d) {
    for (const f of fs.readdirSync(d)) {
        const p = path.join(d, f);
        if (fs.statSync(p).isDirectory()) { scan(p); continue; }
        if (!/\.(c|cpp|h)$/.test(f)) continue;
        scanFile(p);
    }
}
scan(path.join(SRC, 'sw', 'app'));
scan(path.join(SRC, 'sw', 'impl', 'net'));   //net层也发布界面文案(xfer状态/错误)
scanFile(path.join(SRC, 'main.cpp'));
scan(path.join(SRC, 'sd', 'apps'));          //SD应用文案也用固件字库渲染

// 常用界面扩展字(为后续应用预留: 数量词/操作/媒体/系统词)
const extra =
    '一二三四五六七八九十百千万亿零两' +
    '级层个项行条张块片幅页章节组列位' +
    '显隐亮度色温饱和对比锐化静音' +
    '删增改查复制粘贴剪切重命名新建删除清空覆盖除' +
    '确认取消应用保存另打开关闭退出运行停止暂停继续' +
    '贪吃蛇得分重开上下左右结束点击操作规则碰撞' +
    '上一张下一张首末快进快退播放循环随机单曲' +
    '网络连接断信号密码热点流量' +
    '日期钟秒闹表历星期' +
    '安装卸载升级版本更新检查修复恢复出厂' +
    '输入键盘鼠标串网口地址端口协议' +
    '错警告提示注意危险请稍等正在载入初始化' +
    '图片照片视频音乐文档压缩包程序脚本' +
    '排序刷详情属性帮助关于反馈' +
    '选择全选反已清统计' +
    '简繁英文法语德日语俄韩' +
    '清晰流畅标清高清超画质' +
    '温湿气压感光照噪声陀螺加速计' +
    '灯继电舵马步进伺服' +
    '健康心率血压' +
    '推理模型' +
    '结果趋势均值峰值耗时总';

for (const ch of extra) set.add(ch);
console.log([...set].join(''));
console.error('total: ' + set.size);
