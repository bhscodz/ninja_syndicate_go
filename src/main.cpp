#include <SFML/Graphics.hpp>
#include <optional>
using namespace sf;
int main()

{
    
    Vector2u windowSize(800,600);
    sf::VideoMode videoMode(windowSize);
    sf::RenderWindow window(videoMode, "Hello World");
    window.setFramerateLimit(60);

    // player
    RectangleShape player;
    player.setSize(Vector2f(50.f,50.f));
    player.setPosition(Vector2f(375.f,275.f));
    player.setFillColor(Color(70,180,110));
    const float playerSpeed = 250.f;
    Clock clock;


    while (window.isOpen())
    {
        Time frameTime = clock.restart();
        float deltaTime = frameTime.asSeconds();
        
        std::optional<sf::Event> event = window.pollEvent();

        while (event.has_value())
        {
            if (event->is<sf::Event::Closed>())
            {
                window.close();
                break;
            }

            event = window.pollEvent();
        }

        if (!window.isOpen())
        {
            break;
        }
        Vector2f movement(0.f,0.f);
        if ( Keyboard::isKeyPressed(Keyboard::Key::W)||
            Keyboard::isKeyPressed(Keyboard::Key::Up)){
                movement.y=movement.y-1.f;
            }
        if(Keyboard::isKeyPressed(Keyboard::Key::S)||
            Keyboard::isKeyPressed(Keyboard::Key::Down)){
                movement.y=movement.y+.1f;
            }
        if(Keyboard::isKeyPressed(Keyboard::Key::A)||
            Keyboard::isKeyPressed(Keyboard::Key::Left)){
                movement.x=movement.x-.1f;
            }
        if(Keyboard::isKeyPressed(Keyboard::Key::D)||
            Keyboard::isKeyPressed(Keyboard::Key::Right)){
                movement.x=movement.x+.1f;
            }
        float movementLength =std::sqrt(
            movement.x*movement.x+
            movement.y*movement.y
        );
        if (movementLength>0.f){
            movement.x=movement.x/movementLength;
            movement.y=movement.y/movementLength;
        }
        float movementDistance=playerSpeed*deltaTime;
        movement.x=movement.x*movementDistance;
        movement.y=movement.y*movementDistance;
        player.move(movement);
        window.clear(sf::Color(25,30,45));
        window.draw(player);
        window.display();
    }

    return 0;
}