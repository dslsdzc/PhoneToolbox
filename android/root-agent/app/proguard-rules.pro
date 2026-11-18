# Manifest 里的组件必须保留；其余类允许 R8 混淆/裁剪。
-keep class com.local.service.MainActivity { *; }
