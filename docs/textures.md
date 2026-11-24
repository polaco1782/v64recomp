# Desktop texture packs

<a id="gles3-texture-packs-and-editor"></a>
<a id="gles2-texture-packs-and-editor"></a>


Desktop GLES3 loads Rice/GLideN64 PNG ZIPs directly. On the first draw of a
texture, it checks the original texture and palette loads against the Rice
filenames in the ZIP. It remembers matches in memory. Exact IDs, CI palette or texture `$` wildcards,
and split `_rgb` / `_a` images are supported. Palette matching checks both
the GLideN64 texture-filter palette and the palette bytes transferred by the RDP.
Local editor overrides take precedence.
GLES decodes matched PNGs on a background worker. It draws the original until
the replacement is ready, then uploads it on the render thread. A limit on uploads
per display list spreads mipmap generation over several draws as a scene loads.
Uploaded replacements use a separate cache that grows on demand up to 256 MiB,
including mipmaps. F5 clears the cache and discards outstanding requests.
Tiles with incomplete source loads or incompatible images keep their original
appearance.

```sh
./build/banjo-kazooie-us-gles-release/bin/banjo-kazooie-us
# Or supply a compatible pack at launch:
./build/banjo-kazooie-us-gles-release/bin/banjo-kazooie-us --texture-pack /path/to/pack.zip
```

Press **F8** to pause and open a gallery of recently seen, editable textures.
It is available by default on desktop in Debug and Release, without a ZIP.
Set `RECOMP_GLES2_TEXTURE_EDITOR=0` to skip observing thumbnails. Passing
`--texture-editor` or `--texture-pack ZIP` explicitly overrides this setting.
Select a thumbnail, drop a PNG onto the window, then press **Esc** or **F8**
to resume. The replacement needs the same aspect ratio as the original, and
must fit the GPU limit (at most 2048 px per side and 16 MiB on disk).
GLES3 applies it to every use of that texture. It saves a local override in
`<data>/texture-overrides/<hash>.gles.png` without touching the source ZIP;
delete that override and reload to revert. **F5** refreshes the matches and
switches between original and replacement textures. This editor selects
textures from thumbnails, not by clicking game geometry.

Texture support is enabled by default on desktop. Set
`-DRECOMP_GLES2_TEXTURE_PACKS=OFF` to omit the loader and editor.
WebAssembly builds omit both.
