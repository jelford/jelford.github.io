
## Building a better blog: organising output

In the last entry, I walked through using a simple make-file to generate the
HTML for a simple blog - it will convert a collection of markdown files into
HTML snippets, then concatenate them into a single "blog" page.

In this entry, we'll build on that by managing multiple entries more in a nicer
way:

- we'll make it put them into some sort of order (based on when they were written)
- we'll style the resulting page to make it easier to see where one entry stops
and the next one starts

### Faster feedback

#### See it in a browser

First, let's get ourselves set up with a fast feedback loop. We'd like to see
fully-rendered output, as it'll look to readers, in a browser, immediately as
we make updates.

We can open up out rendered output in a web browser easily enough:

    firefox ./blog.html

... but most browsers will behave a little differently when we view `file:///`
URLs compared to if they were on the web. We can get the "real thing" without
much extra hassle though:

    python3 -m http.server --bind 127.0.0.1

will run a simple http server that serves up the current working directory. The
`--bind` on the end instructs it to only listen for connections from `localhost`
- it's not much, but it's probably best not to open up a socket to the whole
world if we don't have to. If you're using an older version of python you can do:

    python2 -m SimpleHTTPServer

#### Make all the time

If we were building a JavaScript app, we'd probably have set up a file watch
by now. We should do that here:

    watch make

That'll run `make` every couple of seconds. It's not as event-driven as we might
be used to from `inotify`-type file watching, but it'll do us. Now, every time
we save a file, we should see the update in our browsers as soon as we refresh.

#### See it straight away

One last thing: while we're working on things, let's just have our browser
auto-refresh the page.

We'll put this in the `<head>` of our HTML:

    <meta http-equiv="refresh" content="5" >

So our final make step now looks like this:

    blog.html : $(blog_objects) 
    	echo '<html><head><link rel="stylesheet" type="text/css" href="style.css" ><meta http-equiv="refresh" content="2" ></head><body>' > blog.html 
    	cat $(blog_objects) >> blog.html 
    	echo '</body></html>' >> blog.html

Now, nobody like pages that auto-refresh while they're looking at them, so I'd
like to take this out before pushing anything up, but there are a couple of things
that mean it wouldn't be _the worst thing ever_ if we forget:

- It looks like (Firefox at least) it nice enough to maintain your postion on
the page between refreshes
- Because we're generating a static page (that github's going to server statically),
every request should actually hit the cache (unless there's been a real change),
so it won't put as much extra load on the network/server as you might expect.

Which is kind of nice...

### Move our compilation into a script

I don't like writing raw bash (or makefiles, for that matter) any more than the
next person. Let's move our bash step into a separate script:

    blog.html : $(blog_objects)
    	./compile_blog $(blog_objects)

    --- ./compile_blog ---
    #! /usr/bin/env sh
    echo '<html><head><link rel="stylesheet" type="text/css" href="style.css" ><meta http-equiv="refresh" content="2" ></head><body>' > blog.html 
    cat $@ >> blog.html 
    echo '</body></html>' >> blog.html

Phew. That's better. Okay, now let's move to using a nicer language than `sh`
for manipulating files. We'll use python, since that's what I like.

~~~
#! /usr/bin/env python3

import argparse
import sys

def parse_args(args):
	parser = argparse.ArgumentParser(description='Combine blog posts into a single page')
	parser.add_argument('output_file', type=argparse.FileType('w'))
	parser.add_argument('input_files', nargs=argparse.REMAINDER)

	args = parser.parse_args(sys.argv[1:])
	return args.output_file, args.input_files


if __name__ == '__main__':
	output_file, input_files = parse_args(sys.argv[1:])
		
	output_file.write('<html><head><link rel="stylesheet" type="text/css" href="style.css" ><meta http-equiv="refresh" content="2" ></head><body>')

	for entry_path in input_files:
		with open(entry_path, 'r') as blog_entry:
			output_file.write(blog_entry.read())

	output_file.write('</body></html>')
~~~

Finally, let's make it so that our main page re-compiles any time we make
changes to its build script:

    blog.html : $(blog_objects) compile_blog
    	...

### Ordering our blog posts

Now we're in proper programming land, we can start to be more sophisticated
with how we work with our files.

Let's start by moving them into some sort of very basic domain model:

~~~
class BlogPost():
	def __init__(self, file_path):
		self.file_path = file_path

	def content(self):
		with open(self.file_path, 'r') as f:
			return f.read()

def compile(output_file, blog_posts):
	output_file.write('<html><head><link rel="stylesheet" type="text/css" href="style.css" ><meta http-equiv="refresh" content="2" ></head><body>')

	for b in blog_posts:
		output_file.write(b.content())

	output_file.write('</body></html>')

def compile_files(output_file, input_paths):
	compile(output_file, (BlogPost(p) for p in input_paths))

if __name__ == '__main__':
	compile_files(*parse_args(sys.argv[1:]))
~~~

Now we've got a model, we can sort the blog posts by the details - in this
case, I want the time the file was create (or, added to git, as a reasonable
proxy, since I want it to survive renamed, checkouts elsewhere, that sort
of thing).

~~~
import subprocess
import datetime
import timezone


class BlogPost():
	...
	def timestamp(self):
		timestamp = subprocess.check_output(['git', 'log', '--diff-filter=A', '--pretty=%aD' , '--', self.file_path]).decode().strip()
		return datetime.strptime(timestamp, '%a, %d %b %Y %H:%M:%S %z') if timestamp \
			else datetime.now(tz=timezone.utc) # if the file's not yet in git

...

def compile(...):
	...
	for b in reversed(sorted(blog_posts, key=lambda b: b.timestamp())):
		...
~~~

Finally, let's put something around each post, so it's easier to see when one
post stops and the next one starts:

~~~
def compile(...):
	...
	for b in ...
		output_file.write('<section class="blog_post">')
		output_file.write(b.content())
		output_file.write('</section>')
~~~

And we'll put something in `style.css` to make that visible:

~~~
section.blog_post {
	border-top: 1px solid grey;
}
~~~
