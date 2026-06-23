import csv
import random

def generate_orderbook_data(filename="market_data.csv", num_rows=1000000):
    print(f"Generating {num_rows} rows of market data into {filename}...")
    
    active_orders = [] 
    next_order_id = 1
    mid_price = 100 # Starting middle price
    
    with open(filename, mode='w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['Action', 'OrderId', 'Side', 'Price', 'Quantity'])
        
        for i in range(num_rows):
            # 80% chance to Add, 20% chance to Cancel
            # (Only cancel if we actually have active orders to cancel)
            if active_orders and random.random() < 0.2:
                # Pick a random active order to cancel
                idx = random.randint(0, len(active_orders) - 1)
                order_id = active_orders.pop(idx)
                
                # C, OrderId,,, 
                writer.writerow(['C', order_id, '', '', ''])
            else:
                order_id = next_order_id
                next_order_id += 1
                
                side = 'B' if random.random() < 0.5 else 'S'
                
                # Generate a price clustered around the mid_price using Gaussian/Normal distribution
                if side == 'B':
                    # Bids sit slightly below the mid-price
                    price = int(random.gauss(mid_price - 1, 3))
                else:
                    # Asks sit slightly above the mid-price
                    price = int(random.gauss(mid_price + 1, 3))
                    
                price = max(1, price) # Prevent negative prices
                qty = random.randint(10, 500)
                
                writer.writerow(['A', order_id, side, price, qty])
                active_orders.append(order_id)
                
                # 1% chance for the mid-price to drift up or down (simulates market movement)
                if random.random() < 0.01:
                    mid_price += random.choice([-1, 1])
                    mid_price = max(10, mid_price) # Keep price strictly positive

            # Print progress
            if (i + 1) % 200000 == 0:
                print(f"[{i + 1}/{num_rows}] rows generated...")

    print("Done!")

if __name__ == "__main__":
    # Generate 1 Million rows (change this number if you want a smaller/larger dataset)
    generate_orderbook_data("market_data.csv", 3000000)
