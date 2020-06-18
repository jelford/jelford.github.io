---
layout: default.liquid
---

# <a href="/index.html">~jelford</a>/blog:

[feed](/rss.xml)

----

{% for post in collections.posts.pages %}
* <a href="{{post.permalink}}">{{post.title}}</a>
  {{post.excerpt}}
{% endfor %}
