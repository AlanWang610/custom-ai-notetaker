#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// Simple JSON string builder
std::string escapeJsonString(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::string buildJsonRequest(const std::string& text) {
    std::string escapedText = escapeJsonString(text);
    return "{\"contents\":[{\"parts\":[{\"text\":\"Please provide a concise summary of the following text:\\n\\n" + escapedText + "\"}]}],\"generationConfig\":{\"temperature\":0.7,\"maxOutputTokens\":1000}}";
}

// Read API key from .env file
std::string readApiKey() {
    std::ifstream envFile(".env", std::ios::binary);
    if (!envFile.is_open()) {
        std::cerr << "Error: Could not open .env file" << std::endl;
        return "";
    }
    
    // Read the entire file
    std::string content((std::istreambuf_iterator<char>(envFile)), std::istreambuf_iterator<char>());
    
    // Remove BOM if present
    if (content.length() >= 3 && 
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content = content.substr(3);
    }
    
    // Split into lines
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        // Remove carriage return if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        if (line.find("GEMINI_API_KEY=") == 0) {
            return line.substr(15); // Remove "GEMINI_API_KEY=" prefix
        }
    }
    std::cerr << "Error: GEMINI_API_KEY not found in .env file" << std::endl;
    return "";
}

// Read text file content
std::string readTextFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <text_file>" << std::endl;
        return 1;
    }
    
    std::string filename = argv[1];
    std::string textContent = readTextFile(filename);
    
    if (textContent.empty()) {
        std::cerr << "Error: Could not read file or file is empty" << std::endl;
        return 1;
    }
    
    std::string apiKey = readApiKey();
    if (apiKey.empty()) {
        std::cerr << "Error: Could not read GEMINI_API_KEY from .env file" << std::endl;
        return 1;
    }
    
    std::string endpoint = "https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent";
    
    // Create JSON payload for Gemini API using simple string building
    std::string body = buildJsonRequest(textContent);
    std::string url_with_key = endpoint + "?key=" + apiKey;
    
    // Use WinHTTP for the request
    HINTERNET hSession = WinHttpOpen(L"SummaryApp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        std::cerr << "Error: WinHttpOpen failed" << std::endl;
        return 1;
    }
    
    // Parse URL manually for HTTPS
    std::string host = "generativelanguage.googleapis.com";
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::string path = "/v1beta/models/gemini-1.5-flash:generateContent?key=" + apiKey;
    
    // Connect to server
    HINTERNET hConnect = WinHttpConnect(hSession, std::wstring(host.begin(), host.end()).c_str(), port, 0);
    if (!hConnect) {
        std::cerr << "Error: WinHttpConnect failed" << std::endl;
        WinHttpCloseHandle(hSession);
        return 1;
    }
    
    // Create request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", std::wstring(path.begin(), path.end()).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        std::cerr << "Error: WinHttpOpenRequest failed" << std::endl;
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }
    
    // Set headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    
    std::cout << "Sending request to Gemini API..." << std::endl;
    
    // Send request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.c_str(), body.length(), body.length(), 0)) {
        std::cerr << "Error: WinHttpSendRequest failed" << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }
    
    // Receive response
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        std::cerr << "Error: WinHttpReceiveResponse failed" << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }
    
    // Read response data
    std::string response_string;
    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    LPSTR pszOutBuffer;
    
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            std::cerr << "Error: WinHttpQueryDataAvailable failed" << std::endl;
            break;
        }
        
        pszOutBuffer = new char[dwSize + 1];
        ZeroMemory(pszOutBuffer, dwSize + 1);
        
        if (!WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
            std::cerr << "Error: WinHttpReadData failed" << std::endl;
            delete[] pszOutBuffer;
            break;
        }
        
        response_string.append(pszOutBuffer, dwDownloaded);
        delete[] pszOutBuffer;
        
    } while (dwSize > 0);
    
    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    // Parse response - simple string parsing
    std::cout << "\n=== SUMMARY ===" << std::endl;
    
    // Look for the text content in the response - find "text": "content"
    size_t textStart = response_string.find("\"text\": \"");
    if (textStart != std::string::npos) {
        textStart += 9; // Skip past "\"text\": \""
        size_t textEnd = response_string.find("\"", textStart);
        if (textEnd != std::string::npos) {
            std::string summary = response_string.substr(textStart, textEnd - textStart);
            
            // Unescape the text
            std::string unescaped;
            for (size_t i = 0; i < summary.length(); i++) {
                if (summary[i] == '\\' && i + 1 < summary.length()) {
                    switch (summary[i + 1]) {
                        case 'n': unescaped += '\n'; i++; break;
                        case 'r': unescaped += '\r'; i++; break;
                        case 't': unescaped += '\t'; i++; break;
                        case '\\': unescaped += '\\'; i++; break;
                        case '"': unescaped += '"'; i++; break;
                        default: unescaped += summary[i]; break;
                    }
                } else {
                    unescaped += summary[i];
                }
            }
            
            std::cout << unescaped << std::endl;
        } else {
            std::cerr << "Error: Could not find end of text in response" << std::endl;
            std::cout << "Raw response: " << response_string << std::endl;
        }
    } else {
        std::cerr << "Error: Could not find text content in response" << std::endl;
        std::cout << "Raw response: " << response_string << std::endl;
    }
    
    return 0;
}
