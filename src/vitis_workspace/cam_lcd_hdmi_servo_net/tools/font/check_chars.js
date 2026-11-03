//临时校验脚本: 提取C源码字符串字面量中的汉字, 与字库源文件比对
const fs = require('fs');
const file = process.argv[2];
const src = fs.readFileSync(file, 'utf8');
//去块注释与行注释, 只留代码
const code = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
const chars = new Set();
const re = /"((?:[^"\\]|\\.)*)"/g;
let m;
while ((m = re.exec(code)) !== null) {
  for (const ch of m[1]) if (/[\u4e00-\u9fff]/.test(ch)) chars.add(ch);
}
const font = fs.readFileSync('src/sw/impl/lvgl/fonts/font_cn_16.c', 'utf8');
const missing = [...chars].filter(ch => !font.includes(ch));
console.log(file);
console.log('  UI字符串汉字数:', chars.size);
console.log(missing.length ? '  MISSING: ' + missing.join(' ') : '  FONT OK: 全部覆盖');
