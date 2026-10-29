from pathlib import Path
import re,ast,ctypes,json
s=Path('Balisto_Live_BMS_Dashboard_Names.ino').read_text(encoding='utf-8')
formats=[]
for m in re.finditer(r'apiAppend\(((?:"(?:[^"\\]|\\.)*"\s*)+)',s):
 formats.append(''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"',m.group(1))))
assert len(formats)==3,formats
snprintf=ctypes.CDLL('msvcrt')._snprintf
snprintf.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_char_p]
snprintf.restype=ctypes.c_int
S=lambda x:ctypes.c_char_p(x.encode())
U=lambda x:ctypes.c_uint(x)
L=lambda x:ctypes.c_ulong(x)
I=lambda x:ctypes.c_int(x)
def format_c(fmt,*args):
 buf=ctypes.create_string_buffer(4096)
 n=snprintf(buf,len(buf),fmt.encode(),*args)
 assert 0<=n<len(buf),n
 return buf.value.decode()
for total in [0,1,8,9,93,105]:
 for offset in range(0,total+1,8):
  end=min(offset+8,total)
  body=format_c(formats[0],S('BMS-2026-10-16-R7'),S('Reading ff:ff:ff:ff:ff:ff'),L(0xffffffff),L(0xffffffff),L(total),U(total),U(offset),I(end if end<total else -1),S('false'))
  for i in range(offset,end):
   body+=format_c(formats[1],S('' if i==offset else ','),S('ff:ff:ff:ff:ff:ff'),S('SP16S60A'+'9'*32),I(-127),I(100),ctypes.c_double(655.35),U(65535),S('true'),S('false'),S('FFE1 characteristic unavailable'),L(4294967),L(4294967))
  body+=formats[2]
  assert len(body.encode())<4096,len(body)
  parsed=json.loads(body)
  assert len(parsed['batteries'])==end-offset
print('PASS: Actual C printf formats produce valid bounded JSON for empty, full, final, and out-of-range pages (up to 105 devices).')
