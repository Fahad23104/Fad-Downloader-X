let mediaPanel = null;

// --- 1. NETWORK SNIFFER PANEL ---
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request.action === "show_media_panel") {
        createMediaPanel(request.url);
    }
});

function createMediaPanel(url) {
    if (!mediaPanel) {
        mediaPanel = document.createElement('div');
        mediaPanel.id = 'fdx-media-sniffer-panel';
        mediaPanel.innerHTML = `<strong>FDX Sniffer</strong><br><span style="font-size:11px;">Media Stream Detected!</span><hr style="margin:5px 0; border:0; border-top:1px solid #444;">`;
        document.body.appendChild(mediaPanel);
        
        // Add Close Button
        let closeBtn = document.createElement('span');
        closeBtn.innerHTML = '✖';
        closeBtn.style.cssText = 'position:absolute;top:5px;right:10px;cursor:pointer;color:#ff4444;font-size:14px;';
        closeBtn.onclick = () => mediaPanel.style.display = 'none';
        mediaPanel.appendChild(closeBtn);
    }
    
    mediaPanel.style.display = 'block';

    // Prevent duplicate buttons for the same stream URL
    let existing = Array.from(mediaPanel.querySelectorAll('button')).find(b => b.dataset.url === url);
    if (existing) return;

    let btn = document.createElement('button');
    btn.className = 'fdx-stream-btn';
    btn.dataset.url = url;
    
    let shortName = url.split('/').pop().split('?')[0] || "media_stream";
    if (shortName.length > 25) shortName = shortName.substring(0, 25) + "...";
    btn.innerText = "📥 " + shortName;

    btn.onclick = () => {
        chrome.runtime.sendMessage({ action: "download_sniffed_media", url: url });
        btn.innerText = "✅ Sent to FDX!";
        btn.style.backgroundColor = "#28a745";
        setTimeout(() => { mediaPanel.style.display = 'none'; }, 2000);
    };

    mediaPanel.appendChild(btn);
}

// --- 2. DIRECT DOM VIDEO OVERLAY ---
function injectDownloadButtons() {
    const videos = document.querySelectorAll('video');
    videos.forEach(video => {
        if (video.dataset.fdxAttached) return;
        video.dataset.fdxAttached = "true"; // Mark as attached

        const wrapper = document.createElement('div');
        wrapper.className = 'fdx-video-wrapper';
        video.parentNode.insertBefore(wrapper, video);
        wrapper.appendChild(video);

        const btn = document.createElement('button');
        btn.className = 'fdx-download-btn';
        btn.innerHTML = '📥 Download Video';
        
        wrapper.addEventListener('mouseenter', () => btn.style.display = 'block');
        wrapper.addEventListener('mouseleave', () => btn.style.display = 'none');

        btn.addEventListener('click', (e) => {
            e.preventDefault(); 
            e.stopPropagation();
            
            let videoUrl = video.src || video.currentSrc;
            if (videoUrl && videoUrl.startsWith('http')) {
                chrome.runtime.sendMessage({ action: "download_sniffed_media", url: videoUrl });
                btn.innerHTML = '✅ Sent!';
                btn.style.backgroundColor = '#28a745';
            } else {
                alert("FDX: This is a hidden blob stream. Look for the FDX Sniffer Panel in the top right corner for the true download link!");
            }
        });
        wrapper.appendChild(btn);
    });
}

// Scan the page every 2 seconds to catch videos that load late
setInterval(injectDownloadButtons, 2000);