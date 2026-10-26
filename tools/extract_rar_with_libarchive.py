import ctypes, os
L=ctypes.CDLL('libarchive.so.13')
L.archive_read_new.restype=ctypes.c_void_p
for f in ('archive_read_support_format_all','archive_read_support_filter_all'):
    getattr(L,f).argtypes=[ctypes.c_void_p]
L.archive_read_open_filename.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t]
L.archive_read_next_header.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
L.archive_entry_pathname.argtypes=[ctypes.c_void_p]; L.archive_entry_pathname.restype=ctypes.c_char_p
L.archive_read_data.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]; L.archive_read_data.restype=ctypes.c_ssize_t
L.archive_error_string.argtypes=[ctypes.c_void_p]; L.archive_error_string.restype=ctypes.c_char_p
a=L.archive_read_new()
L.archive_read_support_format_all(a);L.archive_read_support_filter_all(a)
assert L.archive_read_open_filename(a,b'/mnt/user-data/uploads/reports.rar',10240)==0
e=ctypes.c_void_p()
while L.archive_read_next_header(a,ctypes.byref(e))==0:
    n=L.archive_entry_pathname(e).decode()
    buf=bytearray();tmp=ctypes.create_string_buffer(65536)
    while True:
        r=L.archive_read_data(a,tmp,65536)
        if r<=0:
            if r<0: print('ERR',L.archive_error_string(a))
            break
        buf+=tmp.raw[:r]
    if n.endswith('/'): continue
    os.makedirs(os.path.dirname(n) or '.',exist_ok=True)
    open(n,'wb').write(buf);print(n,len(buf))
