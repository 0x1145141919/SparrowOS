本文档为initramfs的构建配置文件规范文档，以指导构造器构建initramfs
配置文件使用json,其中有files数组，表项定义为{"dest_path","src_base","src_relative_path"}
可以有多个文本变量，以便弹入src_base
需要out_put_path{"src_base","src_relative_path"}输出的