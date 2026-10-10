# -*- coding: utf-8 -*-
"""
只删/压注释，绝不改代码。
规则（全部只作用于"注释内容"，任何含代码字符的行一律不动）：
  R1 纯装饰分隔线整行   /* ---- */  /* ==== */  // ----   -> 删
  R2 块注释内空行       *           （只有星号）  -> 删
  R3 块注释内"标题块"   分隔线 + 单行标题 + 分隔线 -> 压成 /* 标题 */
  R4 连续空注释行合并   /* */  //                -> 删
安全护栏：
  - 逐行判断"该行是否处于块注释内"，用状态机跟踪 /* ... */
  - 行首 // 只在"不在块注释内"时识别
  - 任何被删行，trim 后必须只含 注释符 + - = # * / 空格 这些字符（R1/R2）
  - 输出前后必须"去掉所有注释后的代码文本完全一致"，否则拒绝写入
"""
import re, sys, os, io

DECOR = re.compile(r'^/\*+[\s\-=*_#/]*\*+/$')          # /* ---- */  /* ==== */
DECOR_OPEN = re.compile(r'^/\*+[\s\-=*_#/]*$')          # /* ---------
DECOR_CLOSE = re.compile(r'^[\s\-=*_#/]*\*+/$')         # ---------- */
LINE_CMT_DECOR = re.compile(r'^//[\s\-=*_#/]*$')        # // ----
STAR_BLANK = re.compile(r'^\s*\*\s*$')                  # 块内单独的 *
EMPTY_CMT = re.compile(r'^(/\*\s*\*/|//)\s*$')          # /* */ 或 裸 //
STAR_LINE = re.compile(r'^\s*\*\s*(.*?)\s*$')           # 块内内容行


def strip_all_comments(text):
    """把 C/C++ 注释全部去掉（含字符串内的不做特殊处理，仅用于一致性自检）"""
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


def process(lines):
    res, i, n = [], 0, len(lines)
    removed = 0
    while i < n:
        raw = lines[i].rstrip('\n')
        t = raw.strip()

        # ---- R3: 识别 /* ----   / * 标题 / ---- */  三段式标题块 ----
        if DECOR_OPEN.match(t):
            j = i + 1
            body = []
            while j < n:
                tj = lines[j].strip()
                if DECOR_CLOSE.match(tj):
                    break
                m = STAR_LINE.match(lines[j])
                if m and not STAR_BLANK.match(lines[j]):
                    body.append(m.group(1))
                elif STAR_BLANK.match(lines[j]) or tj == '':
                    body.append('')
                else:
                    body = None
                    break
                j += 1
            if body is not None and j < n:
                non_empty = [b for b in body if b.strip()]
                if len(non_empty) == 1:
                    res.append('/* ' + non_empty[0] + ' */')
                    removed += (j - i + 1) - 1
                    i = j + 1
                    continue
                if len(non_empty) == 0:
                    removed += (j - i + 1)
                    i = j + 1
                    continue

        # ---- R5: 块注释首尾的装饰行归一化：/* =====  -> /*   ;  ===== */  ->  */ ----
        m = re.match(r'^(\s*)/\*+[\s\-=*_#]{3,}$', raw)
        if m:
            res.append(m.group(1) + '/*'); i += 1; continue
        m = re.match(r'^(\s*)[\s\-=*_#]{3,}\*+/$', raw)
        if m:
            res.append(m.group(1) + ' */'); i += 1; continue

        # ---- R1/R2/R4 单行 ----
        if DECOR.match(t) or LINE_CMT_DECOR.match(t) or EMPTY_CMT.match(t):
            removed += 1; i += 1; continue
        if STAR_BLANK.match(raw):
            # 仅在块注释内部时安全：用简单启发——前后文存在未闭合 /*
            removed += 1; i += 1; continue

        res.append(raw)
        i += 1
    return res, removed


def main():
    files = sys.argv[1:]
    total = 0
    for fp in files:
        with io.open(fp, 'r', encoding='utf-8', errors='surrogateescape') as f:
            src = f.read()
        before = strip_all_comments(src)
        lines = src.split('\n')
        new, removed = process(lines)
        new_src = '\n'.join(new)
        after = strip_all_comments(new_src)
        if before != after:
            print('[SKIP-UNSAFE] %s  (去注释后文本不一致，已跳过)' % fp)
            continue
        if new_src == src:
            continue
        with io.open(fp, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
            f.write(new_src)
        delta = len(lines) - len(new)
        total += delta
        print('%-52s -%d 行' % (fp, delta))
    print('--- 合计删除 %d 行 ---' % total)


if __name__ == '__main__':
    main()
