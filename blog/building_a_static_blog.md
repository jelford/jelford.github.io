% Building a static blog with Markdown, Make, and Python

## Background

I wanted to build a simple way to publish blog posts. I'm happy without any
server-side gubbins; all I want it a simple place to put some static content,
so github-pages is fine.

The other must for me is to be able to use markdown to write in. I've tried
just doing raw HTML in the past, and frankly it doesn't fill me with joy.

Github has a tool built in, which you can install locally and test out - [Jekyll](https://help.github.com/articles/using-jekyll-with-pages/)
- but I wanted to use pandoc for compiling my markdown to HTML, and besides
I don't have ruby installed.

## Managing the build

All the build needs to do is:

- figure out all the markdown files
- convert them into HTML
- compile that list into a single front-page blog.html

Sounds like a job for make. Besides, that gives me a chance to brush up on
Makefiles.

The rest of ths post goes through each of those steps, one by one. For reference,
this whole site is checked in as a github page; you can easily check out the source
for yourself [here](https://github.com/jelford/jelford.github.io).

## Gathering the markdown files

Make has built-in macros for grouping together a series of files from one place
into a build target:

    blog_sources := $(wildcard blog/*.md)
    blog_outputs := $(patsubst %.md,%.html,$(blog_sources))

Those will translate to something like:

    blog_sources := page_1.md this_blogpost.md that_blogpost.md
    blog_outputs := page_1.html this_blogpost.html that_blogpost.html

Using these macros, we can define a set of make rules to compile the blog pages,
and then generate a single front-page that combines them together for easy
browsing:

    blog.html : $(blog_outputs)
    	combine_pages $(blog_outputs)
    
    $(blog_outputs) : $(blog_sources)
    	convert_to_html $< $@

So then we just have to implement the conversion and combining steps.

## Converting to HTML

I'll be using pandoc to convert markdown files to HTML. This couldn't be eaiser:

    blog/%.html : blog/%.md 
    	pandoc --email-obfuscation=javascript $< -f markdown -t html5 -o $@

Notice this doesn't use `$(blog_outputs)` or `$(blog_sources)`. I found make was
desperate to do _all_ the `$(blog_outputs)` at once in the case they were the make
target (makes sense), so this is a [pattern rule](http://www.gnu.org/software/make/manual/make.html#Pattern-Intro).

## Combining

Here's the simplest implementation I can think of for creating an easy-to-browse
front page from all the inputs:

    blog.html : $(blog_objects)
    	echo '<html><head><link rel="stylesheet" type="text/css" href="style.css" ></head><body>' > blog.html
    	cat $(blog_objects) >> blog.html
    	echo '</body></html>' >> blog.html

That'll just concatenate all the pages together (in any old order) into one long
page with all the content. Pretty spartan, but it does the job. Just one more
thing so we don't all hate ourselves every time we load the page: adding a style
sheet:

    blog.html : $(blog_outputs)
    	echo '<html><head><link rel="stylesheet" type="text/css" href="style.css" ></head><body>' > blog.html
    	cat $(blog_objects) >> blog.html
    	echo '</body></html>' >> blog.html


Over the course of the next few blog posts, I'll about adding nicities on top of this starting
point:

- moving the combination step out of make into a script
- ordering the posts by some sort of date
- making the page load dynamically, so you don't just get everything on there
as a one-er each time
