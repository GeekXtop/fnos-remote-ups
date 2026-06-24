#!/bin/sh

# Check if the first argument is a command
if [ "${1:0:1}" != "-" ]; then
  if [ -e "$1" ] || command -v "$1" >/dev/null; then
    exec "$@"
  fi
fi

if [ -n "$REMOTE_UPS" ]; then
  set -- "$@" -u "$REMOTE_UPS"
fi

if [ -n "$SERVER_PORT" ]; then
  set -- "$@" -p "$SERVER_PORT"
fi

if [ -n "$DEVICE_MANUFACTURER" ]; then
  set -- "$@" --manufacturer "$DEVICE_MANUFACTURER"
fi

if [ -n "$DEVICE_PRODUCT" ]; then
  set -- "$@" --product "$DEVICE_PRODUCT"
fi

if [ -n "$DEVICE_VENDOR_ID" ]; then
  set -- "$@" --vendor-id "$DEVICE_VENDOR_ID"
fi

if [ -n "$DEVICE_PRODUCT_ID" ]; then
  set -- "$@" --product-id "$DEVICE_PRODUCT_ID"
fi

if [ "$AUTO_MOUNT" == "true" ]; then
  set -- "$@" -m
else
  if [ -n "$AUTO_MOUNT" ]; then
    set -- "$@" -m "$AUTO_MOUNT"
  fi
fi


echo "Starting fnos-virtual-ups server.."
echo "Server arguments: $@"
exec /usr/sbin/fnos-remote-ups "$@"
