////////////////////////////////////////////
//  Filename NFCHttpClient.cpp
//  Copyright (C) 2017  Stonexin
//  CreateTime 2017/07/05
//
////////////////////////////////////////////
#include "NFCHttpClient.h"

#if _MSC_VER
#define snprintf _snprintf
#endif

bool NFCHttpClient::Execute()
{
    if (m_pBase)
    {
        event_base_loop(m_pBase, EVLOOP_ONCE | EVLOOP_NONBLOCK);
    }

    return true;
}

bool NFCHttpClient::Init()
{
#if NF_PLATFORM == NF_PLATFORM_WIN
    WORD wVersionRequested;
    WSADATA wsaData;
    wVersionRequested = MAKEWORD(2, 2);
    int err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        return false;
    }
#endif

#if NF_ENABLE_SSL

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // Initialize OpenSSL
    SSL_library_init();
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    m_pSslCtx = SSL_CTX_new(SSLv23_client_method());
    if (!m_pSslCtx) {
        printf("SSL_CTX_new err.");
        return false;
    }

#endif

    m_pBase = event_base_new();
    if (m_pBase == nullptr)
    {
        printf("event_base_new err.");
        return false;
    }
    return true;
}

bool NFCHttpClient::Final()
{
    if (m_pBase)
    {
        event_base_free(m_pBase);
        m_pBase = nullptr;
    }

#if NF_ENABLE_SSL
    if (m_pSslCtx)
    {
        SSL_CTX_free(m_pSslCtx);
        m_pSslCtx = nullptr;
    }

#if (OPENSSL_VERSION_NUMBER < 0x10100000L) || defined(LIBRESSL_VERSION_NUMBER)
    EVP_cleanup();
    ERR_free_strings();

#if OPENSSL_VERSION_NUMBER < 0x10000000L
    ERR_remove_state(0);
#else
    ERR_remove_thread_state(NULL);
#endif

    CRYPTO_cleanup_all_ex_data();

    sk_SSL_COMP_free(SSL_COMP_get_compression_methods());
#endif /* (OPENSSL_VERSION_NUMBER < 0x10100000L) || defined(LIBRESSL_VERSION_NUMBER) */
#endif
    return true;
}

bool NFCHttpClient::MakeRequest(const std::string& strUri,
                                HTTP_RESP_FUNCTOR_PTR pCB,
                                const std::string& strPostData,
                                const std::map<std::string, std::string>& xHeaders,
                                const NFHttpType eHttpType,
								const NFGUID id)
{
    struct evhttp_uri* http_uri = NULL;
    const char* scheme, * host, * path, * query;

    http_uri = evhttp_uri_parse(strUri.c_str());
    if (http_uri == NULL)
    {
        return false;
    }

    bool isHttps = false;

    scheme = evhttp_uri_get_scheme(http_uri);
    std::string lowwerScheme(scheme);
    std::transform(lowwerScheme.begin(), lowwerScheme.end(), lowwerScheme.begin(), (int (*)(int)) std::tolower);
    if (scheme == NULL || (lowwerScheme.compare("https") != 0 && lowwerScheme.compare("http") != 0))
    {
        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }

        return false;
    }

    if (lowwerScheme.compare("https") == 0)
    {
        isHttps = true;
    }

    host = evhttp_uri_get_host(http_uri);
    if (host == NULL)
    {
        printf("url must have a host \n");
        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }
        return false;
    }

    int port = evhttp_uri_get_port(http_uri);
    if (port == -1)
    {
        port = isHttps ? 443 : 80;
    }

    path = evhttp_uri_get_path(http_uri);
    if (strlen(path) == 0)
    {
        path = "/";
    }

    char uri[512] = {0};

    query = evhttp_uri_get_query(http_uri);
    if (query == NULL)
    {
        snprintf(uri, sizeof(uri) - 1, "%s", path);
    }
	else
    {
        snprintf(uri, sizeof(uri) - 1, "%s?%s", path, query);
    }
    uri[sizeof(uri) - 1] = '\0';

    //-------we do not verify peer--------//
    //like the curl SSL_VERIFYPEER is set false

    //if (1 != SSL_CTX_load_verify_locations(ssl_ctx, crt, NULL)) {
    //	err_openssl("SSL_CTX_load_verify_locations");
    //	goto error;
    //}
    //SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    //SSL_CTX_set_cert_verify_callback(ssl_ctx, cert_verify_callback,(void *)host);

    struct bufferevent* bev = NULL;


#if NF_ENABLE_SSL
    SSL *pSSL = SSL_new(m_pSslCtx);
    if (pSSL == NULL)
    {
        printf("SSL_new err.");
        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }

        return false;
    }
#endif

    if (!isHttps)
    {
        bev = bufferevent_socket_new(m_pBase, -1, BEV_OPT_CLOSE_ON_FREE);
    } 
	else
    {
#if NF_ENABLE_SSL
        bev = bufferevent_openssl_socket_new(m_pBase, -1, pSSL,
            BUFFEREVENT_SSL_CONNECTING,
            BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
#endif
    }

    if (bev == NULL)
    {
        fprintf(stderr, "bufferevent_socket_new() failed\n");

        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }

        return false;
    }

#if NF_ENABLE_SSL
    if (isHttps)
    {
        bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
    }
#endif

    struct evhttp_connection* evcon = evhttp_connection_base_bufferevent_new(m_pBase, NULL, bev, host, port);
    if (evcon == NULL)
    {
        fprintf(stderr, "evhttp_connection_base_bufferevent_new() failed\n");

        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }

        return false;
    }

    if (m_nRetry > 0)
    {
        evhttp_connection_set_retries(evcon, m_nRetry);
    }
    if (m_nTimeOut >= 0)
    {
        evhttp_connection_set_timeout(evcon, m_nTimeOut);
    }

    HttpObject* pHttpObj = new HttpObject(this, bev, pCB, id);

    // Fire off the request
    struct evhttp_request* req = evhttp_request_new(OnHttpReqDone, pHttpObj);
    if (req == NULL)
    {
        fprintf(stderr, "evhttp_request_new() failed\n");

        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }

        return false;
    }

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(output_headers, "Host", host);
    evhttp_add_header(output_headers, "Connection", "close");
    //evhttp_add_header(output_headers, "Connection", "keep-alive");

    std::map<std::string, std::string>::const_iterator it = xHeaders.cbegin();
    while (it != xHeaders.cend())
    {
        evhttp_add_header(output_headers, it->first.c_str(), it->second.c_str());
        it++;
    }

    size_t nLen = strPostData.length();
    if (nLen > 0)
    {
        struct evbuffer* output_buffer = evhttp_request_get_output_buffer(req);
        evbuffer_add(output_buffer, strPostData.c_str(), nLen);
        char buf[256] = {0};
        evutil_snprintf(buf, sizeof(buf) - 1, "%lu", (unsigned long) nLen);
        evhttp_add_header(output_headers, "Content-Length", buf);
    }

    int r_ = evhttp_make_request(evcon, req, (evhttp_cmd_type)eHttpType, uri);
    if (r_ != 0)
    {
        fprintf(stderr, "evhttp_make_request() failed\n");

        if (http_uri)
        {
            evhttp_uri_free(http_uri);
        }

        return false;
    }

    if (http_uri)
    {
        evhttp_uri_free(http_uri);
    }

    return true;
}

bool NFCHttpClient::DoGet(const std::string& strUri, HTTP_RESP_FUNCTOR_PTR pCB,
                               const std::map<std::string, std::string>& xHeaders, const NFGUID id)
{
    return MakeRequest(strUri, pCB, "", xHeaders, NFHttpType::NF_HTTP_REQ_GET);
}

bool NFCHttpClient::DoPost(const std::string& strUri, const std::string& strPostData, HTTP_RESP_FUNCTOR_PTR pCB,
                                const std::map<std::string, std::string>& xHeaders, const NFGUID id)
{
    return MakeRequest(strUri, pCB, strPostData, xHeaders, NFHttpType::NF_HTTP_REQ_POST);
}

void NFCHttpClient::OnHttpReqDone(struct evhttp_request* req, void* ctx)
{
    HttpObject* pHttpObj = (HttpObject*) (ctx);

    if (req == NULL)
    {
        /* If req is NULL, it means an error occurred, but
        * sadly we are mostly left guessing what the error
        * might have been.  We'll do our best... */
        struct bufferevent* bev = (struct bufferevent*) ctx;
        unsigned long oslerr = 0;
        int printed_err = 0;
        int errcode = EVUTIL_SOCKET_ERROR();

        std::string strErrMsg = "";
        fprintf(stderr, "some request failed - no idea which one though!\n");
        /* Print out the OpenSSL error queue that libevent
        * squirreled away for us, if any. */

        char buffer[1024] = {0};
        int nread = 0;

#if NF_ENABLE_SSL
        while ((oslerr = bufferevent_get_openssl_error(bev))) {
            ERR_error_string_n(oslerr, buffer, sizeof(buffer));
            fprintf(stderr, "%s\n", buffer);
            strErrMsg += std::string(buffer);
            printed_err = 1;
        }
#endif
        /* If the OpenSSL error queue was empty, maybe it was a
        * socket error; let's try printing that. */
        if (!printed_err)
        {
            char tmpBuf[1024] = {0};
            snprintf(tmpBuf, 1024, "socket error = %s (%d)\n",
                     evutil_socket_error_to_string(errcode),
                     errcode);
            strErrMsg += std::string(tmpBuf);
        }

        
        throw std::runtime_error(strErrMsg);

        return;
    }

    int nRespCode = evhttp_request_get_response_code(req);
    char buffer[4096] = {0};
    int nread = 0;
    std::string strResp;
    while ((nread = evbuffer_remove(evhttp_request_get_input_buffer(req),
                                    buffer, sizeof(buffer)))
           > 0)
    {
        //TODO it's not good idea,to append or memcpy
        strResp += std::string(buffer, nread);
    }

    if (req->evcon)
    {
        evhttp_connection_free(req->evcon);
    }

    if (req->output_headers)
    {
        evhttp_clear_headers(req->output_headers);
    }

    if (pHttpObj)
    {
        if (pHttpObj->m_pBev)
        {
#if NF_ENABLE_SSL
            SSL* pSSL = bufferevent_openssl_get_ssl(pHttpObj->m_pBev);
            SSL_free(pSSL);
#endif
        }
    }

    if (pHttpObj->m_pCB)
    {
        if (pHttpObj->m_pCB.get())
        {
            HTTP_RESP_FUNCTOR fun(*pHttpObj->m_pCB.get());
            fun(pHttpObj->mID, nRespCode, strResp);
        }
    }

    //it shoue be push back to the pool
	delete pHttpObj;
}
