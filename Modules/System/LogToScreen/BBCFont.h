
// Taken from ??

// dd if=../RISCOS.img bs=1 skip=$(( 0x585D8 )) count=$(( 0xFC058CD8 - 0xFC0585D8 )) > HardFont
// xxd -c 8 -i HardFont BBCFont.c
// Inserted const to put into .text

unsigned char const HardFont[][8] = {
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00 },
  { 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x36, 0x36, 0x7f, 0x36, 0x7f, 0x36, 0x36, 0x00 },
  { 0x0c, 0x3f, 0x68, 0x3e, 0x0b, 0x7e, 0x18, 0x00 },
  { 0x60, 0x66, 0x0c, 0x18, 0x30, 0x66, 0x06, 0x00 },
  { 0x38, 0x6c, 0x6c, 0x38, 0x6d, 0x66, 0x3b, 0x00 },
  { 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x0c, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0c, 0x00 },
  { 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x18, 0x30, 0x00 },
  { 0x00, 0x18, 0x7e, 0x3c, 0x7e, 0x18, 0x00, 0x00 },
  { 0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 },
  { 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },
  { 0x00, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x00, 0x00 },
  { 0x3c, 0x66, 0x6e, 0x7e, 0x76, 0x66, 0x3c, 0x00 },
  { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7e, 0x00 },
  { 0x3c, 0x66, 0x06, 0x0c, 0x18, 0x30, 0x7e, 0x00 },
  { 0x3c, 0x66, 0x06, 0x1c, 0x06, 0x66, 0x3c, 0x00 },
  { 0x0c, 0x1c, 0x3c, 0x6c, 0x7e, 0x0c, 0x0c, 0x00 },
  { 0x7e, 0x60, 0x7c, 0x06, 0x06, 0x66, 0x3c, 0x00 },
  { 0x1c, 0x30, 0x60, 0x7c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x7e, 0x06, 0x0c, 0x18, 0x30, 0x30, 0x30, 0x00 },
  { 0x3c, 0x66, 0x66, 0x3c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x3c, 0x66, 0x66, 0x3e, 0x06, 0x0c, 0x38, 0x00 },
  { 0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00 },
  { 0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30 },
  { 0x0c, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0c, 0x00 },
  { 0x00, 0x00, 0x7e, 0x00, 0x7e, 0x00, 0x00, 0x00 },
  { 0x30, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x30, 0x00 },
  { 0x3c, 0x66, 0x0c, 0x18, 0x18, 0x00, 0x18, 0x00 },
  { 0x3c, 0x66, 0x6e, 0x6a, 0x6e, 0x60, 0x3c, 0x00 },
  { 0x3c, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66, 0x00 },
  { 0x7c, 0x66, 0x66, 0x7c, 0x66, 0x66, 0x7c, 0x00 },
  { 0x3c, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3c, 0x00 },
  { 0x78, 0x6c, 0x66, 0x66, 0x66, 0x6c, 0x78, 0x00 },
  { 0x7e, 0x60, 0x60, 0x7c, 0x60, 0x60, 0x7e, 0x00 },
  { 0x7e, 0x60, 0x60, 0x7c, 0x60, 0x60, 0x60, 0x00 },
  { 0x3c, 0x66, 0x60, 0x6e, 0x66, 0x66, 0x3c, 0x00 },
  { 0x66, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66, 0x00 },
  { 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e, 0x00 },
  { 0x3e, 0x0c, 0x0c, 0x0c, 0x0c, 0x6c, 0x38, 0x00 },
  { 0x66, 0x6c, 0x78, 0x70, 0x78, 0x6c, 0x66, 0x00 },
  { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7e, 0x00 },
  { 0x63, 0x77, 0x7f, 0x6b, 0x6b, 0x63, 0x63, 0x00 },
  { 0x66, 0x66, 0x76, 0x7e, 0x6e, 0x66, 0x66, 0x00 },
  { 0x3c, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x7c, 0x66, 0x66, 0x7c, 0x60, 0x60, 0x60, 0x00 },
  { 0x3c, 0x66, 0x66, 0x66, 0x6a, 0x6c, 0x36, 0x00 },
  { 0x7c, 0x66, 0x66, 0x7c, 0x6c, 0x66, 0x66, 0x00 },
  { 0x3c, 0x66, 0x60, 0x3c, 0x06, 0x66, 0x3c, 0x00 },
  { 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
  { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x00 },
  { 0x63, 0x63, 0x6b, 0x6b, 0x7f, 0x77, 0x63, 0x00 },
  { 0x66, 0x66, 0x3c, 0x18, 0x3c, 0x66, 0x66, 0x00 },
  { 0x66, 0x66, 0x66, 0x3c, 0x18, 0x18, 0x18, 0x00 },
  { 0x7e, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x7e, 0x00 },
  { 0x7c, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7c, 0x00 },
  { 0x00, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x00, 0x00 },
  { 0x3e, 0x06, 0x06, 0x06, 0x06, 0x06, 0x3e, 0x00 },
  { 0x3c, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },
  { 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x60, 0x60, 0x7c, 0x66, 0x66, 0x66, 0x7c, 0x00 },
  { 0x00, 0x00, 0x3c, 0x66, 0x60, 0x66, 0x3c, 0x00 },
  { 0x06, 0x06, 0x3e, 0x66, 0x66, 0x66, 0x3e, 0x00 },
  { 0x00, 0x00, 0x3c, 0x66, 0x7e, 0x60, 0x3c, 0x00 },
  { 0x1c, 0x30, 0x30, 0x7c, 0x30, 0x30, 0x30, 0x00 },
  { 0x00, 0x00, 0x3e, 0x66, 0x66, 0x3e, 0x06, 0x3c },
  { 0x60, 0x60, 0x7c, 0x66, 0x66, 0x66, 0x66, 0x00 },
  { 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3c, 0x00 },
  { 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x70 },
  { 0x60, 0x60, 0x66, 0x6c, 0x78, 0x6c, 0x66, 0x00 },
  { 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },
  { 0x00, 0x00, 0x36, 0x7f, 0x6b, 0x6b, 0x63, 0x00 },
  { 0x00, 0x00, 0x7c, 0x66, 0x66, 0x66, 0x66, 0x00 },
  { 0x00, 0x00, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x00, 0x00, 0x7c, 0x66, 0x66, 0x7c, 0x60, 0x60 },
  { 0x00, 0x00, 0x3e, 0x66, 0x66, 0x3e, 0x06, 0x07 },
  { 0x00, 0x00, 0x6c, 0x76, 0x60, 0x60, 0x60, 0x00 },
  { 0x00, 0x00, 0x3e, 0x60, 0x3c, 0x06, 0x7c, 0x00 },
  { 0x30, 0x30, 0x7c, 0x30, 0x30, 0x30, 0x1c, 0x00 },
  { 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3e, 0x00 },
  { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x00 },
  { 0x00, 0x00, 0x63, 0x6b, 0x6b, 0x7f, 0x36, 0x00 },
  { 0x00, 0x00, 0x66, 0x3c, 0x18, 0x3c, 0x66, 0x00 },
  { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3e, 0x06, 0x3c },
  { 0x00, 0x00, 0x7e, 0x0c, 0x18, 0x30, 0x7e, 0x00 },
  { 0x0c, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0c, 0x00 },
  { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
  { 0x30, 0x18, 0x18, 0x0e, 0x18, 0x18, 0x30, 0x00 },
  { 0x31, 0x6b, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
  { 0x3c, 0x66, 0x60, 0xf8, 0x60, 0x66, 0x3c, 0x00 },
  { 0x1c, 0x36, 0x00, 0x63, 0x6b, 0x7f, 0x63, 0x00 },
  { 0x1c, 0x36, 0x00, 0x6b, 0x6b, 0x7f, 0x36, 0x00 },
  { 0x06, 0x01, 0x06, 0x61, 0x96, 0x60, 0x90, 0x60 },
  { 0x05, 0x05, 0x07, 0x61, 0x91, 0x60, 0x90, 0x60 },
  { 0x3c, 0x66, 0x00, 0x66, 0x3c, 0x18, 0x18, 0x00 },
  { 0x3c, 0x66, 0x00, 0x66, 0x66, 0x3e, 0x06, 0x3c },
  { 0x07, 0x01, 0x02, 0x64, 0x94, 0x60, 0x90, 0x60 },
  { 0x06, 0x09, 0x06, 0x69, 0x96, 0x60, 0x90, 0x60 },
  { 0x06, 0x09, 0x07, 0x61, 0x96, 0x60, 0x90, 0x60 },
  { 0x06, 0x09, 0x0f, 0x69, 0x99, 0x60, 0x90, 0x60 },
  { 0x0e, 0x09, 0x0e, 0x69, 0x9e, 0x60, 0x90, 0x60 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0xdb, 0xdb, 0x00 },
  { 0xf1, 0x5b, 0x55, 0x51, 0x00, 0x00, 0x00, 0x00 },
  { 0xc0, 0xcc, 0x18, 0x30, 0x60, 0xdb, 0x1b, 0x00 },
  { 0x00, 0x00, 0x3c, 0x7e, 0x7e, 0x3c, 0x00, 0x00 },
  { 0x0c, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x0c, 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x0c, 0x18, 0x30, 0x30, 0x18, 0x0c, 0x00 },
  { 0x00, 0x30, 0x18, 0x0c, 0x0c, 0x18, 0x30, 0x00 },
  { 0x1b, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x36, 0x36, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x36, 0x6c },
  { 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00 },
  { 0x77, 0xcc, 0xcc, 0xcf, 0xcc, 0xcc, 0x77, 0x00 },
  { 0x00, 0x00, 0x6e, 0xdb, 0xdf, 0xd8, 0x6e, 0x00 },
  { 0x18, 0x18, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18 },
  { 0x18, 0x18, 0x7e, 0x18, 0x7e, 0x18, 0x18, 0x18 },
  { 0x3c, 0x66, 0x60, 0xf6, 0x66, 0x66, 0x66, 0x00 },
  { 0x3e, 0x66, 0x66, 0xf6, 0x66, 0x66, 0x66, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
  { 0x08, 0x3e, 0x6b, 0x68, 0x6b, 0x3e, 0x08, 0x00 },
  { 0x1c, 0x36, 0x30, 0x7c, 0x30, 0x30, 0x7e, 0x00 },
  { 0x00, 0x66, 0x3c, 0x66, 0x66, 0x3c, 0x66, 0x00 },
  { 0x66, 0x3c, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00 },
  { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 },
  { 0x3c, 0x60, 0x3c, 0x66, 0x3c, 0x06, 0x3c, 0x00 },
  { 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x3c, 0x42, 0x99, 0xa1, 0xa1, 0x99, 0x42, 0x3c },
  { 0x1c, 0x06, 0x1e, 0x36, 0x1e, 0x00, 0x3e, 0x00 },
  { 0x00, 0x33, 0x66, 0xcc, 0xcc, 0x66, 0x33, 0x00 },
  { 0x7e, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00 },
  { 0x3c, 0x42, 0xb9, 0xa5, 0xb9, 0xa5, 0x42, 0x3c },
  { 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x3c, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x7e, 0x00 },
  { 0x38, 0x04, 0x18, 0x20, 0x3c, 0x00, 0x00, 0x00 },
  { 0x38, 0x04, 0x18, 0x04, 0x38, 0x00, 0x00, 0x00 },
  { 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x3e, 0x60 },
  { 0x03, 0x3e, 0x76, 0x76, 0x36, 0x36, 0x3e, 0x00 },
  { 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x30 },
  { 0x10, 0x30, 0x10, 0x10, 0x38, 0x00, 0x00, 0x00 },
  { 0x1c, 0x36, 0x36, 0x36, 0x1c, 0x00, 0x3e, 0x00 },
  { 0x00, 0xcc, 0x66, 0x33, 0x33, 0x66, 0xcc, 0x00 },
  { 0x40, 0xc0, 0x40, 0x48, 0x48, 0x0a, 0x0f, 0x02 },
  { 0x40, 0xc0, 0x40, 0x4f, 0x41, 0x0f, 0x08, 0x0f },
  { 0xe0, 0x20, 0xe0, 0x28, 0xe8, 0x0a, 0x0f, 0x02 },
  { 0x18, 0x00, 0x18, 0x18, 0x30, 0x66, 0x3c, 0x00 },
  { 0x30, 0x18, 0x3c, 0x66, 0x7e, 0x66, 0x66, 0x00 },
  { 0x0c, 0x18, 0x3c, 0x66, 0x7e, 0x66, 0x66, 0x00 },
  { 0x18, 0x66, 0x3c, 0x66, 0x7e, 0x66, 0x66, 0x00 },
  { 0x36, 0x6c, 0x3c, 0x66, 0x7e, 0x66, 0x66, 0x00 },
  { 0x66, 0x00, 0x3c, 0x66, 0x7e, 0x66, 0x66, 0x00 },
  { 0x3c, 0x66, 0x3c, 0x66, 0x7e, 0x66, 0x66, 0x00 },
  { 0x3f, 0x66, 0x66, 0x7f, 0x66, 0x66, 0x67, 0x00 },
  { 0x3c, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3c, 0x60 },
  { 0x30, 0x18, 0x7e, 0x60, 0x7c, 0x60, 0x7e, 0x00 },
  { 0x0c, 0x18, 0x7e, 0x60, 0x7c, 0x60, 0x7e, 0x00 },
  { 0x3c, 0x66, 0x7e, 0x60, 0x7c, 0x60, 0x7e, 0x00 },
  { 0x66, 0x00, 0x7e, 0x60, 0x7c, 0x60, 0x7e, 0x00 },
  { 0x30, 0x18, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x00 },
  { 0x0c, 0x18, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x00 },
  { 0x3c, 0x66, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x00 },
  { 0x66, 0x00, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x00 },
  { 0x78, 0x6c, 0x66, 0xf6, 0x66, 0x6c, 0x78, 0x00 },
  { 0x36, 0x6c, 0x66, 0x76, 0x7e, 0x6e, 0x66, 0x00 },
  { 0x30, 0x18, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x0c, 0x18, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x18, 0x66, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x36, 0x6c, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x66, 0x00, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x00, 0x63, 0x36, 0x1c, 0x1c, 0x36, 0x63, 0x00 },
  { 0x3d, 0x66, 0x6e, 0x7e, 0x76, 0x66, 0xbc, 0x00 },
  { 0x30, 0x18, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x0c, 0x18, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x3c, 0x66, 0x00, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x66, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x00 },
  { 0x0c, 0x18, 0x66, 0x66, 0x3c, 0x18, 0x18, 0x00 },
  { 0xf0, 0x60, 0x7c, 0x66, 0x7c, 0x60, 0xf0, 0x00 },
  { 0x3c, 0x66, 0x66, 0x6c, 0x66, 0x66, 0x6c, 0xc0 },
  { 0x30, 0x18, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x0c, 0x18, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x18, 0x66, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x36, 0x6c, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x66, 0x00, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x3c, 0x66, 0x3c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x00, 0x00, 0x3f, 0x0d, 0x3f, 0x6c, 0x3f, 0x00 },
  { 0x00, 0x00, 0x3c, 0x66, 0x60, 0x66, 0x3c, 0x60 },
  { 0x30, 0x18, 0x3c, 0x66, 0x7e, 0x60, 0x3c, 0x00 },
  { 0x0c, 0x18, 0x3c, 0x66, 0x7e, 0x60, 0x3c, 0x00 },
  { 0x3c, 0x66, 0x3c, 0x66, 0x7e, 0x60, 0x3c, 0x00 },
  { 0x66, 0x00, 0x3c, 0x66, 0x7e, 0x60, 0x3c, 0x00 },
  { 0x30, 0x18, 0x00, 0x38, 0x18, 0x18, 0x3c, 0x00 },
  { 0x0c, 0x18, 0x00, 0x38, 0x18, 0x18, 0x3c, 0x00 },
  { 0x3c, 0x66, 0x00, 0x38, 0x18, 0x18, 0x3c, 0x00 },
  { 0x66, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3c, 0x00 },
  { 0x18, 0x3e, 0x0c, 0x06, 0x3e, 0x66, 0x3e, 0x00 },
  { 0x36, 0x6c, 0x00, 0x7c, 0x66, 0x66, 0x66, 0x00 },
  { 0x30, 0x18, 0x00, 0x3c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x0c, 0x18, 0x00, 0x3c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x3c, 0x66, 0x00, 0x3c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x36, 0x6c, 0x00, 0x3c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x66, 0x00, 0x00, 0x3c, 0x66, 0x66, 0x3c, 0x00 },
  { 0x00, 0x18, 0x00, 0xff, 0x00, 0x18, 0x00, 0x00 },
  { 0x00, 0x02, 0x3c, 0x6e, 0x76, 0x66, 0xbc, 0x00 },
  { 0x30, 0x18, 0x66, 0x66, 0x66, 0x66, 0x3e, 0x00 },
  { 0x0c, 0x18, 0x66, 0x66, 0x66, 0x66, 0x3e, 0x00 },
  { 0x3c, 0x66, 0x00, 0x66, 0x66, 0x66, 0x3e, 0x00 },
  { 0x66, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3e, 0x00 },
  { 0x0c, 0x18, 0x66, 0x66, 0x66, 0x3e, 0x06, 0x3c },
  { 0x60, 0x60, 0x7c, 0x66, 0x7c, 0x60, 0x60, 0x00 },
  { 0x66, 0x00, 0x66, 0x66, 0x66, 0x3e, 0x06, 0x3c }
};
