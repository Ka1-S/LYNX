# LYNX na Microsoft Store — passo a passo (pt-BR)

Tudo o que dá para preparar já está no repositório (pacote MSIX, textos, imagens). Falta o que só você pode fazer:
a conta e os cliques de envio. Detalhes técnicos: `docs/STORE.md`.

## 1. Criar a conta (grátis, uma vez)

1. Abra <https://storedeveloper.microsoft.com> → **Get started** / **Começar** → entre com sua conta Microsoft.
2. Escolha conta **Individual** (pessoa física). Confirme a identidade (documento + selfie). Sem taxa.
3. Espere o e-mail de verificação (pode levar alguns dias).

## 2. Reservar o nome

1. Partner Center → **Aplicativos e jogos** → **Novo produto** → **Aplicativo MSIX ou PWA**.
2. Nome: **LYNX**. Se estiver ocupado, tente nesta ordem: **LYNX DAW**, **LYNX Music Studio**, **LYNX Beat Maker**.
   (Se não for exatamente "LYNX", anote o nome reservado para o passo 3.)

## 3. Copiar a identidade para o GitHub

1. No app → **Gerenciamento do produto** → **Identidade do produto**. Copie:
   `Package/Identity/Name`, `Package/Identity/Publisher`, `Package/Properties/PublisherDisplayName`.
2. GitHub → repositório LYNX → **Settings** → **Secrets and variables** → **Actions** → aba **Variables** →
   **New repository variable**, uma para cada:
   - `MSIX_IDENTITY_NAME` = Package/Identity/Name
   - `MSIX_PUBLISHER` = Package/Identity/Publisher (começa com `CN=`)
   - `MSIX_PUBLISHER_DISPLAY` = PublisherDisplayName
   - `MSIX_DISPLAY_NAME` = o nome reservado (só se não for "LYNX")
3. Me avise: eu gero o pacote de novo (ou ele sai sozinho na próxima versão) — arquivo `LYNX-<versão>-store.msix`
   nos assets do release no GitHub.

## 4. Preencher o envio

No app → **Iniciar envio**:

| Seção | O que pôr |
|---|---|
| Preços e disponibilidade | **Gratuito**; mercados: todos; visibilidade: pública |
| Propriedades | Categoria **Música**; política de privacidade: `https://github.com/Ka1-S/LYNX/blob/main/PRIVACY.md`; site `https://lynx-share.com`; suporte `https://github.com/Ka1-S/LYNX/issues`. "Acessa, coleta ou transmite informações pessoais?" **Sim** — por segurança: o relatório de falha (só com consentimento) pode levar um e-mail se o usuário digitar; a política de privacidade cobre isso |
| Classificação etária | Questionário IARC: categoria **App (não jogo)**; violência, sexo, linguagem, drogas, apostas: **não**; os usuários interagem/compartilham conteúdo entre si: **não**; compartilha localização: **não**; compras digitais: **não**; navegador sem restrição: **não**. Resultado esperado: Livre / 3+ |
| Pacotes | Envie `LYNX-<versão>-store.msix` (baixado do release). Família: Windows 10/11 Desktop |
| Recursos restritos (runFullTrust) | Cole: *"LYNX is a desktop digital audio workstation (Win32/JUCE). It needs full trust to host the user's VST3/CLAP audio plugins, open ASIO/WASAPI audio drivers with low latency and read and write the user's project and sample files anywhere on disk."* |
| Listagens da Store | Inglês: `store/listing.en.md`; Português (Brasil): `store/listing.pt-BR.md`. Capturas: as 5 de `store/screenshots/` com as legendas do arquivo |
| Opções de envio | Notas para a certificação: *"Free open-source DAW. No account or sign-in needed. Open Demo.lynx from the app folder or File › New to test."* |

Clique **Enviar para a Store**. A certificação costuma levar de 1 a 3 dias úteis; se reprovarem algo, o e-mail diz o
quê — me mande o texto.

## 5. Depois que estiver no ar

- Me passe o link da página (`https://apps.microsoft.com/detail/<ID>`): eu ligo o botão **"Microsoft Store
  (recomendado)"** no site (já está pronto num branch) e publico.
- Novas versões: Partner Center → LYNX → **Atualizar** → Pacotes → envie o novo `.msix` → Novidades → Enviar.
- Opcional: Windows → Configurações → Sistema → Para desenvolvedores → **Modo de desenvolvedor** permite eu testar
  o pacote instalado neste PC sem pedir administrador (hoje está desligado; o teste foi feito no GitHub Actions).
