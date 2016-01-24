
## Building a better blog: organising output

In the last entry, I walked through using a simple make-file to generate the
HTML for a simple blog - it will convert a collection of markdown files into
HTML snippets, then concatenate them into a single "blog" page.

In this entry, we'll build on that by managing multiple entries more in a nicer
way:

- we'll make it put them into some sort of order (based on when they were written)
- we'll style the resulting page to make it easier to see where one entry stops
and the next one starts

