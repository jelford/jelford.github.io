document.addEventListener('DOMContentLoaded', function() {
	var code = document.querySelectorAll('pre');
	var worker = new Worker('/js/highlight-worker.js');
	worker.onmessage = function(event) { 
		console.log("received result", event);
		let target = code[event.data.sourceid].lastChild
		target.innerHTML = event.data.content; 
		target.classList.add('hljs');
		
	};
	for (let i=0; i<code.length; ++i) {
		worker.postMessage({content: code[i].lastChild.textContent, sourceid: i});
	}
});
