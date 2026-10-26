# Assumptions

- Input files are gzip-compressed streams of two-byte big-endian length-prefixed records.
- The parser supports the no-tracking-number layouts observed in the sample data and equivalent layouts with a two-byte tracking number for common order-book messages.
- Administrative and reference messages are read for framing and counters but do not mutate the book.
- Prices are Nasdaq-style fixed point values with four decimal places.
- The supplied reference PDF is Nasdaq TotalView-ITCH 5.0 order-level documentation.
- The sample feed this project was built around is dominated by 34-byte `U` records that do not match the standard ITCH Order Replace layout.
- Those 34-byte `U` records are handled as sample-specific aggregate price-level updates, keyed by side, stock, price, and venue bytes.
