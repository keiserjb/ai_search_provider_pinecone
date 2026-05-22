# AI Search Pinecone

Pinecone vector database provider for the Backdrop CMS AI Search module. Stores and queries embeddings in Pinecone to power AI-driven semantic search experiences.

## Requirements

- Backdrop CMS 1.x
- `ai_search` module
- A [Pinecone](https://www.pinecone.io/) account with an existing index
- The [Key](https://backdropcms.org/project/key) module, with keys configured for your Pinecone API key and base URL

## Installation

- Install this module using the official [Backdrop CMS instructions](https://backdropcms.org/user-guide/modules).

## Configuration

1. Create Key entries (Admin -> Configuration -> System -> Keys) for:
   - Your Pinecone API key
   - Your Pinecone base URL (e.g., `https://your-index-xxx.svc.pinecone.io`)
2. Enable this module.
3. Go to Admin -> Configuration -> Search and Metadata -> Search API and create or edit a server.
4. Select **Pinecone** as the service backend.
5. Configure the server settings:
   - **Pinecone API Key** — Select the Key that stores your Pinecone API key.
   - **Pinecone Hostname / Base URL** — Select the Key that stores your Pinecone index base URL.
   - **Namespace** — Optional namespace prefix. The Search API index machine name is always appended automatically.
   - **Default Top-K** — Number of results to return per query. Defaults to `5`.
   - **Embeddings engine** — Select the embedding model; its dimension must match your Pinecone index.
6. Create a Search API index on that server.
7. Index your content to populate the Pinecone index.

## Issues

Bugs and feature requests should be reported in the [Issue Queue](https://github.com/backdrop-contrib/ai_search_provider_pinecone/issues).

## Current Maintainer

[Justin Keiser](https://github.com/keiserjb)

## Credits

- Created for Backdrop CMS by [Justin Keiser](https://github.com/keiserjb).
- Developed with AI assistance.

## License

This project is GPL v2 software. See the LICENSE.txt file in this directory for complete text.
