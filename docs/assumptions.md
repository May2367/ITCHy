# Assumptions

- Input files are gzip-compressed streams of two-byte big-endian length-prefixed records.
- The parser supports the no-tracking-number layouts observed in the sample data and equivalent layouts with a two-byte tracking number for common order-book messages.
- Administrative and reference messages are read for framing and counters but do not mutate the book.
- Prices are Nasdaq-style fixed point values with four decimal places.
- Nasdaq TotalView-ITCH 5.0 order-level documentation is the reference for standard message layouts.
- Raw order-level ITCH data is the primary input target.
- Some Nasdaq sample formats, such as `tvagg.gz`-style aggregate data, contain non-standard aggregate records. The parser treats those as price-level updates keyed by side, stock, price, and venue bytes.
