  1. Keep current reader stable
      - Keep the text-mode reader until the new gfx viewer is better.

  2. Add solar_os_doc model first
      - Blocks/runs shared by reader, docview, EPUB, RTF, Markdown.
      - Blocks: heading, paragraph, list item, quote, pre/code, blank/section break.
      - Runs: plain, bold, italic, code, link target later.
      - Stable anchors should exist from day one so the reader can remember positions across reflow, zoom, and importer changes.

  3. Add Markdown/text importers
      - Import Markdown into solar_os_doc.
      - Import plain text into solar_os_doc with ebook-style paragraph flow.
      - Keep Markdown export out of the near-term reader path.

  4. Add gfx document layout engine
      - Takes solar_os_doc + viewport + zoom.
      - Produces positioned lines/runs.
      - Used by docview first and the future reader replacement.

  5. Build docview prototype
      - Temporary gfx viewer for .txt/.md.
      - Tests layout, zoom, scrolling, saved anchors.
      - Does not replace reader yet.

  6. Harden docview into reader-quality app
      - Saved per-file position using stable anchors.
      - Saved per-file zoom/text size.
      - Better prose wrapping, headings, lists, quotes, and code blocks.
      - Page navigation that feels like an ebook reader, not only a scroll view.

  7. Add solar_os_zip
      - Read-only ZIP service.
      - Useful standalone and required by EPUB.

  8. Add solar_os_markup
      - Shared HTML/XML tokenizer/entities.
      - EPUB XHTML uses it.
      - web can migrate toward it later.

  9. Add importers
      - RTF importer to solar_os_doc.
      - EPUB importer via zip + markup + doc.

  10. Replace reader
      - Once docview handles .txt/.md better than current reader.
      - Migrate old reader position/text-size state to new path + zoom + anchor state.
