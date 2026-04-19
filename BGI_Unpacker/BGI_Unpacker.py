import sys
import struct
import os

def unpack_bgi_arc(arc_path):
    if not os.path.exists(arc_path) or not arc_path.lower().endswith('.arc'):
        print(f"[{arc_path}] 不是有效的 .arc 文件，跳过。")
        return

    out_dir = os.path.splitext(arc_path)[0]
    os.makedirs(out_dir, exist_ok=True)
    print(f"正在解包: {os.path.basename(arc_path)} -> {out_dir}/")

    with open(arc_path, 'rb') as f:
        hdr = f.read(16)
        magic, count = struct.unpack('<12sI', hdr)
        
        if not magic.startswith(b'BURIKO ARC20'):
            print(f"错误: {arc_path} 的文件头不是标准 BURIKO ARC20 格式！")
            return

        entries = []
        for _ in range(count):
            entry_data = f.read(128)
            name = entry_data[:96].split(b'\x00')[0].decode('shift_jis', 'ignore')
            offset, size = struct.unpack('<II', entry_data[96:104])
            entries.append((name, offset, size))

        base_offset = 16 + count * 128
        for name, offset, size in entries:
            f.seek(base_offset + offset)
            file_data = f.read(size)
            
            out_path = os.path.join(out_dir, name)
            with open(out_path, 'wb') as out_f:
                out_f.write(file_data)
        
    print(f"解包完成！成功提取 {count} 个文件。\n")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("BGI Unpacker (BURIKO ARC20) - 拖拽解包工具")
        print("用法: 请将一个或多个 .arc 文件拖拽到本程序图标上。")
        input("按任意键退出...")
        sys.exit(1)

    print("--------------------------------------------------")
    print(" BGI Unpacker 开始工作...")
    print("--------------------------------------------------\n")

    for dropped_file in sys.argv[1:]:
        unpack_bgi_arc(dropped_file)
        
    print("--------------------------------------------------")
    print("所有操作已处理完毕！")
    input("按任意键退出...")
