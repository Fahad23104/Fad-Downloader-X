\# FDX - Fad Downloader-X 🚀



A high-performance, multi-threaded Windows download manager built from scratch in C++. FDX is engineered to maximize network bandwidth utilization while smartly bypassing modern anti-bot server restrictions.



\## ✨ Key Features



\* \*\*Extreme Download Speeds:\*\* Dynamically splits files into up to 8 concurrent threads to saturate your internet connection.

\* \*\*Smart RAM Buffering:\*\* Holds 2MB of data per thread in RAM before flushing to the hard drive, completely eliminating disk I/O bottlenecks and file-lock crashes.

\* \*\*Anti-Bot Stealth Engine:\*\* Uses custom HTTP GET probes to bypass 403/405 errors and force hidden file sizes out of heavily protected CDNs (e.g., Google Video, Hetzner).

\* \*\*Modern Custom UI:\*\* Features a sleek, flat-design dashboard with instant \*\*Dark Mode / Light Mode\*\* toggling.

\* \*\*Intelligent File Sniffing:\*\* Automatically parses server `Content-Type` headers to assign the correct extension (e.g., `.mp4`, `.mkv`, `.zip`) when URLs are heavily obfuscated.

\* \*\*Clipboard Monitoring:\*\* Runs silently in the background and instantly wakes up when a valid download link is copied.

\* \*\*Batch Operations:\*\* Multi-select support to pause, resume, or delete multiple downloads simultaneously.



\## 🛠️ Tech Stack



\* \*\*Language:\*\* C++

\* \*\*GUI Framework:\*\* wxWidgets (Custom themed)

\* \*\*Network Library:\*\* libcurl (Configured for high-throughput TCP Keep-Alive)



\## 👨‍💻 Author



Developed by \*\*M. Fahad Irfan\*\*.

