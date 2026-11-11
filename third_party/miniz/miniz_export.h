/* miniz 上游用 CMake 的 generate_export_header 生成这个文件。
   我们是静态链接，不需要任何可见性修饰，给个空实现就够。 */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT

#endif /* MINIZ_EXPORT_H */
