# 编译源代码生成可执行文件
FROM alpine:3.22 AS build

RUN sed -i 's/dl-cdn.alpinelinux.org/mirrors.ustc.edu.cn/g' /etc/apk/repositories && \
    apk add --no-cache build-base

RUN apk add --no-cache libuv-dev

WORKDIR /build
COPY ./include /build/include
COPY ./src     /build/src
COPY Makefile  /build/Makefile

RUN make



# 构建运行镜像
FROM alpine:3.22

# 设置开发者信息和github地址
LABEL github="https://github.com/GeekXtop/fnos-remote-ups"
LABEL maintainer="Winmin Tam <winmintam@gmail.com>"

# 1. 设置镜像地址, 安装 usbip 工具
RUN sed -i 's/dl-cdn.alpinelinux.org/mirrors.ustc.edu.cn/g' /etc/apk/repositories && \
    apk add --no-cache linux-tools-usbip libstdc++ libuv
# 2. 复制 usb.ids 文件
COPY ./docker/usb.ids /usr/share/hwdata/usb.ids
# 3. 从编译镜像复制执行文件
COPY --from=build /build/dist/fnos-remote-ups /usr/sbin/
# 4. 复制启动脚本
COPY ./docker/entrypoint.sh /entrypoint.sh
# 5. 设置环境变量
ENV REMOTE_UPS="" SERVER_PORT="" AUTO_MOUNT="true" DEVICE_MANUFACTURER="" DEVICE_PRODUCT="" DEVICE_VENDOR_ID="" DEVICE_PRODUCT_ID=""
# 6. 设置启动命令
ENTRYPOINT [ "/entrypoint.sh" ]
