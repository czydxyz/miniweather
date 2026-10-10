# -*- coding: utf-8 -*-
"""
删除"教学/why 型"注释块，只留"维护用"注释。

保留：
  - Doxygen 块 /** ... @brief/@param/@return/@note/@warning ... */   （接口契约）
  - 短块注释（非空行数 <= KEEP_LINES，通常是"这个字段是什么/单位/取值"）
  - 行尾注释（code; /* 单位: 0.1C */）
  - 文件头若无 Doxygen，则保留一块但压缩为前 SUMMARY_LINES 行

删除：
  - 非空行数 > KEEP_LINES 且非 Doxygen 的块注释（讲原理/讲背景/对比方案/举例子）

护栏：写入前比对"去掉全部注释后的代码文本"必须完全一致。
用法：python tools_trim_why.py [--dry-run] file...
"""
import re, sys, io, os

KEEP_LINES = int(os.environ.get('KEEP_LINES', '4'))
DRY = '--dry-run' in sys.argv
FILES = [a for a in sys.argv[1:] if not a.startswith('--')]

TAG = re.compile(r'^\s*\*?\s*@(brief|param|return|retval|note|warning|attention|details|sa)\b', re.I)


def norm_code(text):
    """去掉注释 + 归一化空白，用于"代码是否被动过"的一致性校验"""
    return re.sub(r'\s+', ' ', strip_all_comments(text)).strip()


def strip_all_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i+1] == '*':
            j = text.find('*/', i + 2)
            i = n if j == -1 else j + 2
        elif c == '/' and i + 1 < n and text[i+1] == '/':
            j = text.find('\n', i)
            i = n if j == -1 else j
        elif c == '"':
            out.append(c); i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i+1]); i += 2; continue
                if text[i] == '"':
                    i += 1; break
                i += 1
        elif c == "'":
            out.append(c); i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i+1]); i += 2; continue
                if text[i] == "'":
                    i += 1; break
                i += 1
        else:
            out.append(c); i += 1
    return ''.join(out)


def scan_blocks(lines):
    """返回 [(start, end, is_doxygen, body_lines)]，end 为块最后一行下标（含）"""
    blocks, i, n = [], 0, len(lines)
    while i < n:
        t = lines[i].lstrip()
        if t.startswith('/*'):
            is_doc = t.startswith('/**') and not t.startswith('/***')
            start = i
            if '*/' in lines[i][lines[i].find('/*') + 2:]:
                end = i
            else:
                j = i + 1
                while j < n and '*/' not in lines[j]:
                    j += 1
                end = min(j, n - 1)
            body = []
            for k in range(start, end + 1):
                s = lines[k].strip()
                s = re.sub(r'^/\*+', '', s)
                s = re.sub(r'\*+/$', '', s)
                s = s.strip()
                if s:
                    body.append(s)
            blocks.append((start, end, is_doc, body))
            i = end + 1
        else:
            i += 1
    return blocks


def process(lines):
    kill = set()
    report = []
    blocks = scan_blocks(lines)
    first_block_done = False
    for (s, e, is_doc, body) in blocks:
        if is_doc:
            first_block_done = True
            continue
        if not first_block_done:
            first_block_done = True
            # 文件头：压成前 2 行
            if len(body) > 2:
                report.append((s, e, 'HEAD', body[:2]))
                for k in range(s, e + 1):
                    kill.add(k)
            continue
        if len(body) > KEEP_LINES:
            report.append((s, e, 'WHY', body[:3]))
            for k in range(s, e + 1):
                kill.add(k)
    out = [l for i, l in enumerate(lines) if i not in kill]
    # 清掉因删除产生的 3+ 连续空行
    res, blank = [], 0
    for l in out:
        if l.strip() == '':
            blank += 1
            if blank > 2:
                continue
        else:
            blank = 0
        res.append(l)
    return res, report


def main():
    tot_blocks, tot_lines = 0, 0
    for fp in FILES:
        with io.open(fp, 'r', encoding='utf-8', errors='surrogateescape') as f:
            src = f.read()
        before = norm_code(src)
        lines = src.split('\n')
        new, rep = process(lines)
        new_src = '\n'.join(new)
        if norm_code(new_src) != before:
            print('[SKIP-UNSAFE] %s' % fp)
            continue
        if new_src == src:
            continue
        d = len(lines) - len(new)
        tot_blocks += len(rep); tot_lines += d
        if DRY:
            print('### %s   (-%d 行, %d 块)' % (fp, d, len(rep)))
            for (s, e, kind, sample) in rep[:3]:
                print('   [%s L%d-%d] %s...' % (kind, s + 1, e + 1, ' | '.join(sample)[:110]))
        else:
            with io.open(fp, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
                f.write(new_src)
            print('%-50s -%4d 行 (%d 块)' % (fp, d, len(rep)))
    print('--- 合计 %d 块 / %d 行 ---' % (tot_blocks, tot_lines))


if __name__ == '__main__':
    main()
